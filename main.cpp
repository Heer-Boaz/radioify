#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>



#define MINIAUDIO_IMPLEMENTATION
#define MA_ENABLE_WAV
#define MA_ENABLE_MP3
#define MA_ENABLE_FLAC
#include "miniaudio.h"

struct Options {
  std::string input;
  std::string output;
  int bwHz = 4800;
  // double noise = 0.012; // tuned for a "modern recording through 1938 AM"
  double noise = 0.006; // tuned for a "modern recording through 1938 AM"
  bool mono = true;
  bool play = true;
  bool dry = false;
  bool verbose = false;
};

static void die(const std::string& message) {
  std::cerr << "ERROR: " << message << "\n";
  std::exit(1);
}

static void logLine(const std::string& message) {
  std::cout << message << "\n";
}

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static std::string parseInputPath(int argc, char** argv) {
  if (argc <= 1) return {};
  if (argc > 2) die("Provide a single file or folder path only.");
  return std::string(argv[1]);
}

struct Biquad {
  float b0 = 1.0f;
  float b1 = 0.0f;
  float b2 = 0.0f;
  float a1 = 0.0f;
  float a2 = 0.0f;
  float z1 = 0.0f;
  float z2 = 0.0f;

  float process(float x) {
    float y = b0 * x + z1;
    z1 = b1 * x - a1 * y + z2;
    z2 = b2 * x - a2 * y;
    return y;
  }

  void reset() {
    z1 = 0.0f;
    z2 = 0.0f;
  }

  void setLowpass(float sampleRate, float freq, float q) {
    float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
    float cosw = std::cos(w0);
    float sinw = std::sin(w0);
    float alpha = sinw / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = (1.0f - cosw) / 2.0f / a0;
    b1 = (1.0f - cosw) / a0;
    b2 = (1.0f - cosw) / 2.0f / a0;
    a1 = -2.0f * cosw / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void setHighpass(float sampleRate, float freq, float q) {
    float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
    float cosw = std::cos(w0);
    float sinw = std::sin(w0);
    float alpha = sinw / (2.0f * q);
    float a0 = 1.0f + alpha;
    b0 = (1.0f + cosw) / 2.0f / a0;
    b1 = -(1.0f + cosw) / a0;
    b2 = (1.0f + cosw) / 2.0f / a0;
    a1 = -2.0f * cosw / a0;
    a2 = (1.0f - alpha) / a0;
  }

  void setPeaking(float sampleRate, float freq, float q, float gainDb) {
    float a = std::pow(10.0f, gainDb / 40.0f);
    float w0 = 2.0f * 3.1415926535f * freq / sampleRate;
    float cosw = std::cos(w0);
    float sinw = std::sin(w0);
    float alpha = sinw / (2.0f * q);
    float a0 = 1.0f + alpha / a;
    b0 = (1.0f + alpha * a) / a0;
    b1 = -2.0f * cosw / a0;
    b2 = (1.0f - alpha * a) / a0;
    a1 = -2.0f * cosw / a0;
    a2 = (1.0f - alpha / a) / a0;
  }
};

struct RadioDSP {
  float sampleRate = 48000.0f;
  int channels = 1;
  float noiseWeight = 0.015f;
  float presenceDb = 2.5f;
  float lpHz = 5000.0f;
  float hpHz = 120.0f;

  std::vector<Biquad> hp;
  std::vector<Biquad> lp;
  std::vector<Biquad> peq;

  float env = 0.0f;
  float attack = 0.03f;
  float release = 0.40f;
  float thresholdDb = -18.0f;
  float ratio = 4.0f;
  float limit = 0.98f;

  std::mt19937 rng{0x2a4f5a1u};
  std::uniform_real_distribution<float> dist{-1.0f, 1.0f};

  void init(int ch, float sr, float bw, float presence) {
    channels = ch;
    sampleRate = sr;
    presenceDb = presence;
    lpHz = bw;
    hpHz = 120.0f;
    env = 0.0f;

    hp.assign(channels, {});
    lp.assign(channels, {});
    peq.assign(channels, {});
    for (int i = 0; i < channels; ++i) {
      hp[i].setHighpass(sampleRate, hpHz, 0.707f);
      lp[i].setLowpass(sampleRate, lpHz, 0.707f);
      peq[i].setPeaking(sampleRate, 900.0f, 1.0f, presenceDb);
    }
  }

  float computeGainDb(float envDb) {
    if (envDb <= thresholdDb) return 0.0f;
    float over = envDb - thresholdDb;
    float compressed = over / ratio;
    return (thresholdDb + compressed) - envDb;
  }

  void process(float* samples, uint32_t frames) {
    if (!samples || frames == 0) return;
    float attackCoeff = std::exp(-1.0f / (attack * sampleRate));
    float releaseCoeff = std::exp(-1.0f / (release * sampleRate));
    float noiseAmp = 0.0015f * noiseWeight;

    for (uint32_t f = 0; f < frames; ++f) {
      float peak = 0.0f;
      for (int c = 0; c < channels; ++c) {
        float v = samples[f * channels + c];
        peak = std::max(peak, std::fabs(v));
      }

      if (peak > env) {
        env = attackCoeff * env + (1.0f - attackCoeff) * peak;
      } else {
        env = releaseCoeff * env + (1.0f - releaseCoeff) * peak;
      }

      float envDb = 20.0f * std::log10(std::max(env, 1e-6f));
      float gainDb = computeGainDb(envDb);
      float gain = std::pow(10.0f, gainDb / 20.0f);

      for (int c = 0; c < channels; ++c) {
        float v = samples[f * channels + c];
        v = hp[c].process(v);
        v = lp[c].process(v);
        v = peq[c].process(v);

        v *= gain;

        if (noiseWeight > 0.0f) {
          v += dist(rng) * noiseAmp;
        }

        if (v > limit) v = limit;
        if (v < -limit) v = -limit;
        samples[f * channels + c] = v;
      }
    }
  }
};

static inline float clampf(float x, float a, float b) {
  return std::min(std::max(x, a), b);
}

static inline float db2lin(float db) {
  return std::pow(10.0f, db / 20.0f);
}

static inline float lin2db(float x) {
  return 20.0f * std::log10(std::max(x, 1e-12f));
}

struct Compressor {
  float fs = 48000.0f;
  float thresholdDb = -24.0f;
  float ratio = 4.0f;
  float attackMs = 12.0f;
  float releaseMs = 180.0f;

  float env = 0.0f;
  float gainDb = 0.0f;
  float atkCoeff = 0.0f;
  float relCoeff = 0.0f;
  float gainAtkCoeff = 0.0f;
  float gainRelCoeff = 0.0f;

  void setFs(float newFs) {
    fs = newFs;
    setTimes(attackMs, releaseMs);
  }

  void setTimes(float aMs, float rMs) {
    attackMs = aMs;
    releaseMs = rMs;
    atkCoeff = std::exp(-1.0f / (fs * (attackMs / 1000.0f)));
    relCoeff = std::exp(-1.0f / (fs * (releaseMs / 1000.0f)));
    gainAtkCoeff = std::exp(-1.0f / (fs * (attackMs / 1000.0f)));
    gainRelCoeff = std::exp(-1.0f / (fs * (releaseMs / 1000.0f)));
  }

  void reset() {
    env = 0.0f;
    gainDb = 0.0f;
  }

  float process(float x) {
    float a = std::fabs(x);
    if (a > env) {
      env = atkCoeff * env + (1.0f - atkCoeff) * a;
    } else {
      env = relCoeff * env + (1.0f - relCoeff) * a;
    }

    float levelDb = lin2db(env);
    float targetGrDb = 0.0f;
    if (levelDb > thresholdDb) {
      float over = levelDb - thresholdDb;
      float compressedOver = over / ratio;
      float outDb = thresholdDb + compressedOver;
      targetGrDb = outDb - levelDb;
    }

    if (targetGrDb < gainDb) {
      gainDb = gainAtkCoeff * gainDb + (1.0f - gainAtkCoeff) * targetGrDb;
    } else {
      gainDb = gainRelCoeff * gainDb + (1.0f - gainRelCoeff) * targetGrDb;
    }

    return x * db2lin(gainDb);
  }
};

struct Saturator {
  float drive = 1.35f;
  float mix = 0.45f;

  float process(float x) const {
    float yd = std::tanh(drive * x) / std::tanh(drive);
    return (1.0f - mix) * x + mix * yd;
  }
};

static inline float softClip(float x, float t = 0.98f) {
  float ax = std::fabs(x);
  if (ax <= t) return x;
  float s = (x < 0.0f) ? -1.0f : 1.0f;
  float u = (ax - t) / (1.0f - t);
  float y = t + (1.0f - std::exp(-u)) * (1.0f - t);
  return s * y;
}

struct NoiseHum {
  std::mt19937 rng{0x1938u};
  std::uniform_real_distribution<float> dist{-1.0f, 1.0f};
  Biquad hp;
  Biquad lp;
  float fs = 48000.0f;
  float noiseAmp = 0.015f;
  float humAmp = 0.0015f;
  float humHz = 50.0f;
  float humPhase = 0.0f;
  float scEnv = 0.0f;
  float scAtk = 0.0f;
  float scRel = 0.0f;

  void setFs(float newFs) {
    fs = newFs;
    hp.setHighpass(fs, 500.0f, 0.707f);
    lp.setLowpass(fs, 5500.0f, 0.707f);
    hp.reset();
    lp.reset();
    float atkMs = 10.0f;
    float relMs = 250.0f;
    scAtk = std::exp(-1.0f / (fs * (atkMs / 1000.0f)));
    scRel = std::exp(-1.0f / (fs * (relMs / 1000.0f)));
  }

  void reset() {
    humPhase = 0.0f;
    scEnv = 0.0f;
    hp.reset();
    lp.reset();
  }

  float process(float programSample) {
    float a = std::fabs(programSample);
    if (a > scEnv) {
      scEnv = scAtk * scEnv + (1.0f - scAtk) * a;
    } else {
      scEnv = scRel * scEnv + (1.0f - scRel) * a;
    }

    float start = 0.01f;
    float end = 0.05f;
    float t = clampf((scEnv - start) / (end - start), 0.0f, 1.0f);
    float duckDb = 2.0f;
    float duckGain = db2lin(-duckDb * t);

    float n = dist(rng);
    n = hp.process(n);
    n = lp.process(n);
    n *= noiseAmp * duckGain;

    constexpr float twoPi = 6.283185307f;
    humPhase += twoPi * (humHz / fs);
    if (humPhase > twoPi) humPhase -= twoPi;
    float h = std::sin(humPhase) + 0.35f * std::sin(2.0f * humPhase);
    h *= humAmp;

    return n + h;
  }
};

struct Radio1938 {
  float sampleRate = 48000.0f;
  int channels = 1;
  float bwHz = 4800.0f;
  float noiseWeight = 0.012f;

  Biquad hpf;
  Biquad lpf;
  Biquad midBoost;
  Biquad lowMidDip;
  Biquad presBoost;
  Compressor comp;
  Saturator sat;
  NoiseHum noiseHum;

  void init(int ch, float sr, float bw, float noise) {
    channels = std::max(1, ch);
    sampleRate = sr;
    bwHz = bw;
    noiseWeight = noise;

    float safeBw = std::clamp(bwHz, 4200.0f, 5600.0f);
    hpf.setHighpass(sampleRate, 140.0f, 0.707f);
    lpf.setLowpass(sampleRate, safeBw, 0.707f);
    midBoost.setPeaking(sampleRate, 1500.0f, 1.0f, 4.0f);
    lowMidDip.setPeaking(sampleRate, 420.0f, 1.0f, -2.5f);
    presBoost.setPeaking(sampleRate, 3200.0f, 0.9f, 1.5f);

    comp.setFs(sampleRate);
    comp.thresholdDb = -26.0f;
    comp.ratio = 5.0f;
    comp.setTimes(10.0f, 220.0f);

    sat.drive = 1.40f;
    sat.mix = 0.50f;

    noiseHum.setFs(sampleRate);
    noiseHum.noiseAmp = noiseWeight;
    noiseHum.humHz = 50.0f;
    if (noiseWeight <= 0.0f) {
      noiseHum.humAmp = 0.0f;
    } else {
      float scale = std::clamp(noiseWeight / 0.015f, 0.0f, 2.0f);
      noiseHum.humAmp = 0.0015f * scale;
    }

    reset();
  }

  void reset() {
    hpf.reset();
    lpf.reset();
    midBoost.reset();
    lowMidDip.reset();
    presBoost.reset();
    comp.reset();
    noiseHum.reset();
  }

  void process(float* samples, uint32_t frames) {
    if (!samples || frames == 0) return;
    for (uint32_t f = 0; f < frames; ++f) {
      float inL = samples[f * channels];
      float inR = (channels > 1) ? samples[f * channels + 1] : inL;
      float x = (channels > 1) ? 0.5f * (inL + inR) : inL;

      float y = hpf.process(x);
      y = lpf.process(y);
      y = midBoost.process(y);
      y = lowMidDip.process(y);
      y = presBoost.process(y);
      y = comp.process(y);
      y = sat.process(y);
      y += noiseHum.process(y);
      y = softClip(y, 0.985f);

      for (int c = 0; c < channels; ++c) {
        samples[f * channels + c] = y;
      }
    }
  }
};

static std::string formatTime(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0) return "--:--";
  int total = static_cast<int>(seconds);
  int hours = total / 3600;
  int minutes = (total % 3600) / 60;
  int secs = total % 60;
  if (hours > 0) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", hours, minutes, secs);
    return buf;
  }
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d:%02d", minutes, secs);
  return buf;
}

struct KeyEvent {
  WORD vk = 0;
  char ch = 0;
  DWORD control = 0;
};

struct MouseEvent {
  COORD pos{};
  DWORD buttonState = 0;
  DWORD eventFlags = 0;
  DWORD control = 0;
};

struct InputEvent {
  enum class Type {
    None,
    Key,
    Mouse,
    Resize,
  };

  Type type = Type::None;
  KeyEvent key{};
  MouseEvent mouse{};
  COORD size{};
};

class ConsoleInput {
 public:
  void init() {
    handle_ = GetStdHandle(STD_INPUT_HANDLE);
    if (handle_ == INVALID_HANDLE_VALUE) return;
    if (!GetConsoleMode(handle_, &originalMode_)) return;
    DWORD mode = originalMode_;
    mode |= ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT;
    mode &= ~(ENABLE_QUICK_EDIT_MODE | ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    mode |= ENABLE_EXTENDED_FLAGS;
    if (!SetConsoleMode(handle_, mode)) return;
    active_ = true;
  }

  void restore() {
    if (active_) {
      SetConsoleMode(handle_, originalMode_);
    }
  }

  bool poll(InputEvent& out) {
    if (!active_) return false;
    DWORD count = 0;
    if (!GetNumberOfConsoleInputEvents(handle_, &count) || count == 0) return false;
    while (count > 0) {
      INPUT_RECORD rec{};
      DWORD read = 0;
      if (!ReadConsoleInput(handle_, &rec, 1, &read) || read == 0) return false;
      if (rec.EventType == KEY_EVENT) {
        const auto& kev = rec.Event.KeyEvent;
        if (!kev.bKeyDown) {
          count--;
          continue;
        }
        out.type = InputEvent::Type::Key;
        out.key.vk = kev.wVirtualKeyCode;
        out.key.ch = static_cast<char>(kev.uChar.AsciiChar);
        out.key.control = kev.dwControlKeyState;
        return true;
      }
      if (rec.EventType == MOUSE_EVENT) {
        const auto& mev = rec.Event.MouseEvent;
        out.type = InputEvent::Type::Mouse;
        out.mouse.pos = mev.dwMousePosition;
        out.mouse.buttonState = mev.dwButtonState;
        out.mouse.eventFlags = mev.dwEventFlags;
        out.mouse.control = mev.dwControlKeyState;
        return true;
      }
      if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
        out.type = InputEvent::Type::Resize;
        out.size = rec.Event.WindowBufferSizeEvent.dwSize;
        return true;
      }
      count--;
    }
    return false;
  }

  bool active() const { return active_; }

 private:
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  DWORD originalMode_ = 0;
  bool active_ = false;
};

class ConsoleScreen {
 public:
  bool init() {
    out_ = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out_ == INVALID_HANDLE_VALUE) return false;
    if (!GetConsoleMode(out_, &originalMode_)) return false;
    CONSOLE_CURSOR_INFO cursor{};
    if (GetConsoleCursorInfo(out_, &cursor)) {
      originalCursor_ = cursor;
      cursor.bVisible = FALSE;
      SetConsoleCursorInfo(out_, &cursor);
    }
    updateSize();
    return true;
  }

  void restore() {
    if (out_ != INVALID_HANDLE_VALUE) {
      SetConsoleMode(out_, originalMode_);
      if (originalCursor_.dwSize != 0) {
        SetConsoleCursorInfo(out_, &originalCursor_);
      }
    }
  }

  void updateSize() {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(out_, &info)) {
      width_ = 80;
      height_ = 25;
    } else {
      width_ = info.srWindow.Right - info.srWindow.Left + 1;
      height_ = info.srWindow.Bottom - info.srWindow.Top + 1;
    }
    if (width_ < 1) width_ = 1;
    if (height_ < 1) height_ = 1;
    if (static_cast<int>(buffer_.size()) != width_ * height_) {
      buffer_.assign(static_cast<size_t>(width_ * height_), {});
    }
  }

  int width() const { return width_; }
  int height() const { return height_; }

  void clear(WORD attr) {
    CHAR_INFO cell{};
    cell.Char.AsciiChar = ' ';
    cell.Attributes = attr;
    std::fill(buffer_.begin(), buffer_.end(), cell);
  }

  void writeText(int x, int y, const std::string& text, WORD attr) {
    if (y < 0 || y >= height_) return;
    if (x >= width_) return;
    int pos = y * width_ + std::max(0, x);
    int start = std::max(0, x);
    int maxLen = width_ - start;
    for (int i = 0; i < maxLen && i < static_cast<int>(text.size()); ++i) {
      buffer_[pos + i].Char.AsciiChar = text[static_cast<size_t>(i)];
      buffer_[pos + i].Attributes = attr;
    }
  }

  void writeRun(int x, int y, int len, char ch, WORD attr) {
    if (y < 0 || y >= height_) return;
    if (x >= width_) return;
    int start = std::max(0, x);
    int maxLen = std::min(len, width_ - start);
    int pos = y * width_ + start;
    for (int i = 0; i < maxLen; ++i) {
      buffer_[pos + i].Char.AsciiChar = ch;
      buffer_[pos + i].Attributes = attr;
    }
  }

  void writeChar(int x, int y, char ch, WORD attr) {
    if (y < 0 || y >= height_) return;
    if (x < 0 || x >= width_) return;
    int pos = y * width_ + x;
    buffer_[pos].Char.AsciiChar = ch;
    buffer_[pos].Attributes = attr;
  }

  void draw() {
    if (out_ == INVALID_HANDLE_VALUE) return;
    COORD bufferSize{static_cast<SHORT>(width_), static_cast<SHORT>(height_)};
    COORD bufferCoord{0, 0};
    SMALL_RECT rect{0, 0, static_cast<SHORT>(width_ - 1), static_cast<SHORT>(height_ - 1)};
    WriteConsoleOutput(out_, buffer_.data(), bufferSize, bufferCoord, &rect);
  }

 private:
  HANDLE out_ = INVALID_HANDLE_VALUE;
  DWORD originalMode_ = 0;
  CONSOLE_CURSOR_INFO originalCursor_{};
  int width_ = 80;
  int height_ = 25;
  std::vector<CHAR_INFO> buffer_;
};

static std::string fitLine(const std::string& s, int width) {
  if (width <= 0) return "";
  if (static_cast<int>(s.size()) <= width) return s;
  if (width <= 1) return s.substr(0, static_cast<size_t>(width));
  return s.substr(0, static_cast<size_t>(width - 1)) + "~";
}

static bool isSupportedAudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

static void validateInputFile(const std::filesystem::path& p) {
  if (p.empty()) die("Missing input file path.");
  if (!std::filesystem::exists(p)) die("Input file not found: " + p.string());
  if (std::filesystem::is_directory(p)) die("Input path must be a file: " + p.string());
  if (!isSupportedAudioExt(p)) {
    die("Unsupported input format '" + p.extension().string() + "'. Supported: .wav, .mp3, .flac.");
  }
}

struct FileEntry {
  std::string name;
  std::filesystem::path path;
  bool isDir = false;
};

struct BrowserState {
  std::filesystem::path dir;
  std::vector<FileEntry> entries;
  int selected = 0;
  int scrollRow = 0;
};

static std::vector<FileEntry> listEntries(const std::filesystem::path& dir) {
  std::vector<FileEntry> entries;
  std::vector<FileEntry> items;

  if (dir.has_parent_path() && dir != dir.root_path()) {
    entries.push_back(FileEntry{"..", dir.parent_path(), true});
  }

  try {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      const auto& p = entry.path();
      if (entry.is_directory()) {
        items.push_back(FileEntry{p.filename().string(), p, true});
      } else if (entry.is_regular_file() && isSupportedAudioExt(p)) {
        items.push_back(FileEntry{p.filename().string(), p, false});
      }
    }
  } catch (...) {
    return entries;
  }

  std::sort(items.begin(), items.end(), [](const FileEntry& a, const FileEntry& b) {
    if (a.isDir != b.isDir) return a.isDir > b.isDir;
    return toLower(a.name) < toLower(b.name);
  });

  entries.insert(entries.end(), items.begin(), items.end());
  return entries;
}

static void refreshBrowser(BrowserState& state, const std::string& initialName) {
  state.entries = listEntries(state.dir);
  if (state.entries.empty()) {
    state.selected = 0;
    state.scrollRow = 0;
    return;
  }

  if (state.selected < 0 || state.selected >= static_cast<int>(state.entries.size())) {
    state.selected = 0;
  }
  state.scrollRow = 0;

  if (!initialName.empty()) {
    for (size_t i = 0; i < state.entries.size(); ++i) {
      if (toLower(state.entries[i].name) == toLower(initialName)) {
        state.selected = static_cast<int>(i);
        break;
      }
    }
  }
}

struct GridLayout {
  int rowsVisible = 0;
  int totalRows = 0;
  int cols = 0;
  int colWidth = 0;
  std::vector<std::string> names;
};

static std::string fitName(const std::string& name, int colWidth) {
  int maxLen = colWidth - 2;
  if (maxLen <= 1) return std::string(1, name.empty() ? ' ' : name[0]);
  if (static_cast<int>(name.size()) <= maxLen) return name;
  return name.substr(0, static_cast<size_t>(maxLen - 1)) + "~";
}

static GridLayout buildLayout(const BrowserState& state, int width, int rowsVisible) {
  GridLayout layout;
  layout.names.reserve(state.entries.size());
  int maxName = 0;
  for (const auto& e : state.entries) {
    std::string name = e.name;
    if (e.isDir && name != "..") name += "/";
    layout.names.push_back(name);
    maxName = std::max(maxName, static_cast<int>(name.size()));
  }

  int safeRows = std::max(1, rowsVisible);
  int colWidth = std::min(width, std::max(8, maxName + 3));
  int maxCols = std::max(1, width / colWidth);
  int cols = std::max(1, std::min(maxCols, static_cast<int>((state.entries.size() + safeRows - 1) / safeRows)));
  int totalRows = cols > 0 ? static_cast<int>((state.entries.size() + cols - 1) / cols) : 0;
  int visibleRows = std::min(safeRows, totalRows);

  layout.rowsVisible = visibleRows;
  layout.totalRows = totalRows;
  layout.cols = cols;
  layout.colWidth = colWidth;
  return layout;
}


struct AudioState {
  ma_decoder decoder{};
  ma_device device{};
  RadioDSP dsp{};
  Radio1938 radio1938{};
  std::atomic<bool> paused{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> useRadio1938{true};
  std::atomic<bool> seekRequested{false};
  std::atomic<int64_t> pendingSeekFrames{0};
  std::atomic<uint64_t> framesPlayed{0};
  uint64_t totalFrames = 0;
  uint32_t channels = 1;
  uint32_t sampleRate = 48000;
  bool dry = false;
};

static void dataCallback(ma_device* device, void* output, const void*, ma_uint32 frameCount) {
  auto* state = static_cast<AudioState*>(device->pUserData);
  float* out = static_cast<float*>(output);
  if (!state) return;

  if (state->seekRequested.exchange(false)) {
    int64_t target = state->pendingSeekFrames.load();
    if (target < 0) target = 0;
    ma_decoder_seek_to_pcm_frame(&state->decoder, static_cast<ma_uint64>(target));
    state->framesPlayed.store(static_cast<uint64_t>(target));
    state->finished.store(false);
  }

  if (state->paused.load()) {
    std::fill(out, out + frameCount * state->channels, 0.0f);
    return;
  }

  ma_uint64 framesRead = 0;
  ma_result res = ma_decoder_read_pcm_frames(&state->decoder, out, frameCount, &framesRead);
  if (res != MA_SUCCESS && res != MA_AT_END) {
    state->finished.store(true);
    return;
  }
  if (framesRead < frameCount) {
    std::fill(out + framesRead * state->channels, out + frameCount * state->channels, 0.0f);
  }
  if (res == MA_AT_END || framesRead == 0) {
    state->finished.store(true);
    if (state->totalFrames > 0) {
      state->framesPlayed.store(state->totalFrames);
    }
  }

  if (!state->dry && framesRead > 0) {
    if (state->useRadio1938.load()) {
      state->radio1938.process(out, static_cast<uint32_t>(framesRead));
    } else {
      state->dsp.process(out, static_cast<uint32_t>(framesRead));
    }
  }
  state->framesPlayed.fetch_add(framesRead);
}

static void renderToFile(
  const Options& o,
  const RadioDSP& dspTemplate,
  const Radio1938& radio1938Template,
  bool useRadio1938
) {
  const uint32_t sampleRate = 48000;
  const uint32_t channels = useRadio1938 ? 1 : (o.mono ? 1 : 2);
  ma_decoder decoder{};
  ma_decoder_config decConfig = ma_decoder_config_init(ma_format_f32, channels, sampleRate);
  if (ma_decoder_init_file(o.input.c_str(), &decConfig, &decoder) != MA_SUCCESS) {
    die("Failed to open input for decoding.");
  }

  ma_encoder encoder{};
  ma_encoder_config encConfig = ma_encoder_config_init(ma_encoding_format_wav, ma_format_s16, channels, sampleRate);
  if (ma_encoder_init_file(o.output.c_str(), &encConfig, &encoder) != MA_SUCCESS) {
    ma_decoder_uninit(&decoder);
    die("Failed to open output for encoding.");
  }

  RadioDSP dsp = dspTemplate;
  dsp.init(channels, static_cast<float>(sampleRate), static_cast<float>(o.bwHz), dspTemplate.presenceDb);
  Radio1938 radio1938 = radio1938Template;
  radio1938.init(channels, static_cast<float>(sampleRate), static_cast<float>(o.bwHz), static_cast<float>(o.noise));
  constexpr uint32_t chunkFrames = 1024;
  std::vector<float> buffer(chunkFrames * channels);
  std::vector<int16_t> out(buffer.size());

  while (true) {
    ma_uint64 framesRead = 0;
    ma_result res = ma_decoder_read_pcm_frames(&decoder, buffer.data(), chunkFrames, &framesRead);
    if (framesRead == 0 || res == MA_AT_END) break;

    if (!o.dry) {
      if (useRadio1938) {
        radio1938.process(buffer.data(), static_cast<uint32_t>(framesRead));
      } else {
        dsp.process(buffer.data(), static_cast<uint32_t>(framesRead));
      }
    }

    for (size_t i = 0; i < framesRead * channels; ++i) {
      float v = std::clamp(buffer[i], -1.0f, 1.0f);
      out[i] = static_cast<int16_t>(std::lrint(v * 32767.0f));
    }

    ma_uint64 framesWritten = 0;
    ma_encoder_write_pcm_frames(&encoder, out.data(), framesRead, &framesWritten);
  }

  ma_encoder_uninit(&encoder);
  ma_decoder_uninit(&decoder);
}

int main(int argc, char** argv) {
  Options o;
  o.input = parseInputPath(argc, argv);

  float presenceDb = 2.5f;
  float lpHz = static_cast<float>(o.bwHz);
  const uint32_t sampleRate = 48000;
  const uint32_t baseChannels = o.mono ? 1 : 2;
  uint32_t channels = baseChannels;

  RadioDSP dspTemplate;
  dspTemplate.noiseWeight = static_cast<float>(o.noise);
  dspTemplate.init(channels, static_cast<float>(sampleRate), lpHz, presenceDb);
  Radio1938 radio1938Template;
  radio1938Template.init(channels, static_cast<float>(sampleRate), lpHz, static_cast<float>(o.noise));

  auto defaultOutputFor = [](const std::filesystem::path& input) {
    std::string base = input.stem().string();
    return (input.parent_path() / (base + ".radio.wav")).string();
  };

  std::filesystem::path startDir = std::filesystem::current_path();
  std::string initialName;
  if (!o.input.empty()) {
    std::filesystem::path inputPath(o.input);
    if (std::filesystem::exists(inputPath)) {
      if (std::filesystem::is_directory(inputPath)) {
        startDir = inputPath;
        o.input.clear();
      } else {
        startDir = inputPath.parent_path();
        initialName = inputPath.filename().string();
      }
    }
  }

  BrowserState browser;
  browser.dir = startDir;
  refreshBrowser(browser, initialName);

  ConsoleInput input;
  input.init();

  ConsoleScreen screen;
  screen.init();

  AudioState state{};
  state.channels = channels;
  state.sampleRate = sampleRate;
  state.dry = o.dry;
  state.dsp = dspTemplate;
  state.radio1938 = radio1938Template;
  state.useRadio1938.store(true);

  bool deviceReady = false;
  bool decoderReady = false;
  std::filesystem::path nowPlaying;

  auto initDevice = [&]() -> bool {
    if (deviceReady) return true;
    ma_device_config devConfig = ma_device_config_init(ma_device_type_playback);
    devConfig.playback.format = ma_format_f32;
    devConfig.playback.channels = channels;
    devConfig.sampleRate = sampleRate;
    devConfig.dataCallback = dataCallback;
    devConfig.pUserData = &state;

    if (ma_device_init(nullptr, &devConfig, &state.device) != MA_SUCCESS) {
      return false;
    }
    if (ma_device_start(&state.device) != MA_SUCCESS) {
      ma_device_uninit(&state.device);
      return false;
    }
    deviceReady = true;
    return true;
  };

  auto loadFileAt = [&](const std::filesystem::path& file, uint64_t startFrame) -> bool {
    validateInputFile(file);
    if (deviceReady) {
      ma_device_stop(&state.device);
    }
    if (decoderReady) {
      ma_decoder_uninit(&state.decoder);
      decoderReady = false;
    }

    ma_decoder_config decConfig = ma_decoder_config_init(ma_format_f32, channels, sampleRate);
    if (ma_decoder_init_file(file.string().c_str(), &decConfig, &state.decoder) != MA_SUCCESS) {
      return false;
    }
    decoderReady = true;

    ma_uint64 totalFrames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&state.decoder, &totalFrames) == MA_SUCCESS) {
      state.totalFrames = totalFrames;
    } else {
      state.totalFrames = 0;
    }

    if (state.totalFrames > 0 && startFrame > state.totalFrames) {
      startFrame = state.totalFrames;
    }
    if (startFrame > 0) {
      if (ma_decoder_seek_to_pcm_frame(&state.decoder, static_cast<ma_uint64>(startFrame)) != MA_SUCCESS) {
        startFrame = 0;
      }
    }

    state.framesPlayed.store(startFrame);
    state.seekRequested.store(false);
    state.pendingSeekFrames.store(0);
    state.finished.store(false);
    state.paused.store(false);

    state.channels = channels;
    state.dsp = dspTemplate;
    state.dsp.init(channels, static_cast<float>(sampleRate), lpHz, presenceDb);
    state.radio1938 = radio1938Template;
    state.radio1938.init(channels, static_cast<float>(sampleRate), lpHz, static_cast<float>(o.noise));

    if (!deviceReady) {
      if (!initDevice()) {
        ma_decoder_uninit(&state.decoder);
        decoderReady = false;
        return false;
      }
    } else {
      if (ma_device_start(&state.device) != MA_SUCCESS) {
        ma_decoder_uninit(&state.decoder);
        decoderReady = false;
        return false;
      }
    }

    nowPlaying = file;
    return true;
  };

  auto loadFile = [&](const std::filesystem::path& file) -> bool {
    return loadFileAt(file, 0);
  };

  auto ensureChannels = [&](uint32_t newChannels) -> bool {
    if (newChannels == channels) return true;

    uint64_t resumeFrame = decoderReady ? state.framesPlayed.load() : 0;
    bool hadTrack = decoderReady && !nowPlaying.empty();

    if (deviceReady) {
      ma_device_stop(&state.device);
      ma_device_uninit(&state.device);
      deviceReady = false;
    }
    if (decoderReady) {
      ma_decoder_uninit(&state.decoder);
      decoderReady = false;
    }

    channels = newChannels;
    state.channels = channels;
    dspTemplate.init(channels, static_cast<float>(sampleRate), lpHz, presenceDb);
    radio1938Template.init(channels, static_cast<float>(sampleRate), lpHz, static_cast<float>(o.noise));
    state.dsp = dspTemplate;
    state.radio1938 = radio1938Template;

    if (hadTrack) {
      return loadFileAt(nowPlaying, resumeFrame);
    }
    return true;
  };

  auto renderFile = [&](const std::filesystem::path& file) -> void {
    Options renderOpt = o;
    renderOpt.input = file.string();
    if (renderOpt.output.empty()) renderOpt.output = defaultOutputFor(file);
    input.restore();
    screen.restore();
    logLine("Radioify");
    logLine(std::string("  Mode:   render"));
    logLine(std::string("  Input:  ") + renderOpt.input);
    logLine(std::string("  Output: ") + renderOpt.output);
    logLine("Rendering output...");
    renderToFile(renderOpt, dspTemplate, radio1938Template, state.useRadio1938.load());
    logLine("Done.");
  };

  auto seekBy = [&](int direction) {
    if (!decoderReady) return;
    int64_t deltaFrames = static_cast<int64_t>(direction) * 5 * sampleRate;
    int64_t current = static_cast<int64_t>(state.framesPlayed.load());
    int64_t target = current + deltaFrames;
    if (target < 0) target = 0;
    if (state.totalFrames > 0 && target > static_cast<int64_t>(state.totalFrames)) {
      target = static_cast<int64_t>(state.totalFrames);
    }
    state.pendingSeekFrames.store(target);
    state.seekRequested.store(true);
    state.finished.store(false);
  };

  if (!o.input.empty() && o.play && std::filesystem::exists(o.input)) {
    std::filesystem::path inputPath(o.input);
    if (!std::filesystem::is_directory(inputPath)) {
      loadFile(inputPath);
    }
  }

  const WORD kAttrNormal = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  const WORD kAttrHeader = BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  const WORD kAttrHeaderGlow = BACKGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  const WORD kAttrHeaderHot = BACKGROUND_BLUE | BACKGROUND_INTENSITY | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  const WORD kAttrAccent = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  const WORD kAttrDim = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
  const WORD kAttrDir = FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;
  const WORD kAttrHighlight = BACKGROUND_RED | BACKGROUND_GREEN | BACKGROUND_BLUE | BACKGROUND_INTENSITY;
  const WORD kAttrProgressFill = BACKGROUND_GREEN | BACKGROUND_INTENSITY;
  const WORD kAttrProgressEmpty = BACKGROUND_INTENSITY;
  const WORD kAttrProgressFrame = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY;

  screen.clear(kAttrNormal);
  screen.draw();

  bool running = true;
  bool dirty = true;
  auto lastDraw = std::chrono::steady_clock::now();
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;

  while (running) {
    screen.updateSize();
    int width = std::max(40, screen.width());
    int height = std::max(10, screen.height());
    const int headerLines = 5;
    const int listTop = headerLines + 1;
    const int footerLines = 3;
    int listHeight = height - listTop - footerLines;
    if (listHeight < 1) listHeight = 1;
    GridLayout layout = buildLayout(browser, width, listHeight);
    if (layout.totalRows <= layout.rowsVisible) {
      browser.scrollRow = 0;
    } else {
      int maxScroll = layout.totalRows - layout.rowsVisible;
      browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
    }

    auto seekToRatio = [&](double ratio) {
      if (!decoderReady || state.totalFrames == 0) return;
      ratio = std::clamp(ratio, 0.0, 1.0);
      int64_t target = static_cast<int64_t>(ratio * static_cast<double>(state.totalFrames));
      state.pendingSeekFrames.store(target);
      state.seekRequested.store(true);
      state.finished.store(false);
    };

    InputEvent ev{};
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Resize) {
        dirty = true;
        screen.updateSize();
        continue;
      }
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        bool ctrl = (key.control & ctrlMask) != 0;
        if ((key.vk == 'C' || key.ch == 'c' || key.ch == 'C') && ctrl) {
          running = false;
          break;
        }
        if (key.vk == 'Q' || key.ch == 'q' || key.ch == 'Q') {
          running = false;
          // Clear screen before exiting
          screen.clear(kAttrNormal);
          screen.draw();
          break;
        }
        if (key.vk == VK_BACK) {
          if (browser.dir.has_parent_path()) {
            browser.dir = browser.dir.parent_path();
            browser.selected = 0;
            refreshBrowser(browser, "");
            dirty = true;
          }
          continue;
        }
        if (key.vk == VK_RETURN) {
          int count = static_cast<int>(browser.entries.size());
          if (count > 0) {
            const auto& pick = browser.entries[static_cast<size_t>(browser.selected)];
            if (pick.isDir) {
              browser.dir = pick.path;
              browser.selected = 0;
              refreshBrowser(browser, "");
              dirty = true;
            } else if (o.play) {
              if (loadFile(pick.path)) {
                dirty = true;
              }
            } else {
              renderFile(pick.path);
              return 0;
            }
          }
          continue;
        }
        if (key.vk == VK_SPACE || key.ch == ' ') {
          if (o.play && decoderReady) {
            bool next = !state.paused.load();
            state.paused.store(next);
            dirty = true;
          }
          continue;
        }
        if (key.vk == 'R' || key.ch == 'r' || key.ch == 'R') {
          bool next = !state.useRadio1938.load();
          state.useRadio1938.store(next);
          uint32_t desired = next ? 1u : baseChannels;
          ensureChannels(desired);
          dirty = true;
          continue;
        }
        if (key.vk == VK_LEFT) {
          seekBy(-1);
          dirty = true;
          continue;
        }
        if (key.vk == VK_RIGHT) {
          seekBy(1);
          dirty = true;
          continue;
        }
      }
      if (ev.type == InputEvent::Type::Mouse) {
        const MouseEvent& mouse = ev.mouse;
        bool leftPressed = (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
        if (mouse.eventFlags == MOUSE_WHEELED) {
          int delta = static_cast<SHORT>(HIWORD(mouse.buttonState));
          if (delta != 0) {
            browser.scrollRow -= delta / WHEEL_DELTA;
            dirty = true;
          }
          continue;
        }

        if (leftPressed && (mouse.eventFlags == 0 || mouse.eventFlags == MOUSE_MOVED)) {
          if (progressBarWidth > 0 && mouse.pos.Y == progressBarY && progressBarX >= 0) {
            int rel = mouse.pos.X - progressBarX;
            if (rel >= 0 && rel < progressBarWidth) {
              double denom = static_cast<double>(std::max(1, progressBarWidth - 1));
              double ratio = static_cast<double>(rel) / denom;
              seekToRatio(ratio);
              dirty = true;
              continue;
            }
          }
        }

        int count = static_cast<int>(browser.entries.size());
        if (count == 0) continue;
        int x = mouse.pos.X;
        int y = mouse.pos.Y;
        if (y < listTop || y >= listTop + layout.rowsVisible) continue;
        int row = (y - listTop) + browser.scrollRow;
        int col = layout.colWidth > 0 ? x / layout.colWidth : 0;
        if (col < 0 || col >= layout.cols) continue;
        int idx = col * layout.totalRows + row;
        if (idx < 0 || idx >= count) continue;

        if (mouse.eventFlags == MOUSE_MOVED && !leftPressed) {
          if (browser.selected != idx) {
            browser.selected = idx;
            dirty = true;
          }
          continue;
        }

        if (mouse.eventFlags == 0 && leftPressed) {
          if (browser.selected != idx) {
            browser.selected = idx;
            dirty = true;
          }
          const auto& pick = browser.entries[static_cast<size_t>(browser.selected)];
          if (pick.isDir) {
            browser.dir = pick.path;
            browser.selected = 0;
            refreshBrowser(browser, "");
            dirty = true;
          } else if (o.play) {
            if (loadFile(pick.path)) {
              dirty = true;
            }
          } else {
            renderFile(pick.path);
            return 0;
          }
          continue;
        }
      }
    }

    auto now = std::chrono::steady_clock::now();
    if (dirty || now - lastDraw >= std::chrono::milliseconds(150)) {
      screen.updateSize();
      width = std::max(40, screen.width());
      height = std::max(10, screen.height());
      listHeight = height - listTop - footerLines;
      if (listHeight < 1) listHeight = 1;
      layout = buildLayout(browser, width, listHeight);
      if (layout.totalRows <= layout.rowsVisible) {
        browser.scrollRow = 0;
      } else {
        int maxScroll = layout.totalRows - layout.rowsVisible;
        browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
      }

      screen.clear(kAttrNormal);

      const std::string titleRaw = "Radioify";
      std::string title = titleRaw;
      if (static_cast<int>(title.size()) > width) {
        title = fitLine(titleRaw, width);
      }
      int titleLen = static_cast<int>(title.size());
      int titleX = std::max(0, (width - titleLen) / 2);
      screen.writeRun(0, 0, width, ' ', kAttrHeader);
      double seconds = std::chrono::duration<double>(now.time_since_epoch()).count();
      double speed = 1.6;
      double pulse = 0.5 * (std::sin(seconds * speed) + 1.0);
      WORD titleAttr = kAttrHeader;
      if (pulse > 0.70) {
        titleAttr = kAttrHeaderHot;
      } else if (pulse > 0.40) {
        titleAttr = kAttrHeaderGlow;
      }
      for (int i = 0; i < titleLen; ++i) {
        screen.writeChar(titleX + i, 0, title[static_cast<size_t>(i)], titleAttr);
      }
      screen.writeText(0, 1, fitLine(std::string("  Folder: ") + browser.dir.string(), width), kAttrAccent);
      if (o.play) {
        screen.writeText(0, 2, fitLine("  Mouse=select  Click=play/enter  Backspace=up  Click+drag bar=seek  Space=pause  Left/Right=seek  R=toggle  Q=quit", width), kAttrNormal);
      } else {
        screen.writeText(0, 2, fitLine("  Mouse=select  Click=render/enter  Backspace=up  R=toggle  Q=quit", width), kAttrNormal);
      }
      std::string filterLabel = state.useRadio1938.load() ? "1938 radio" : "classic";
      screen.writeText(0, 3, fitLine(std::string("  Filter: ") + filterLabel, width), kAttrDim);
      screen.writeText(0, 4, fitLine("  Showing: folders + .wav/.mp3/.flac", width), kAttrDim);

      if (browser.entries.empty()) {
        screen.writeText(2, listTop, "(no supported files)", kAttrDim);
      } else {
        for (int r = 0; r < layout.rowsVisible; ++r) {
          int y = listTop + r;
          int logicalRow = r + browser.scrollRow;
          if (logicalRow >= layout.totalRows) continue;
          for (int c = 0; c < layout.cols; ++c) {
            int idx = c * layout.totalRows + logicalRow;
            if (idx >= static_cast<int>(browser.entries.size())) continue;
            const auto& entry = browser.entries[static_cast<size_t>(idx)];
            bool isSelected = (idx == browser.selected);
            std::string cell = fitName(layout.names[static_cast<size_t>(idx)], layout.colWidth);
            if (static_cast<int>(cell.size()) < layout.colWidth) {
              cell.append(static_cast<size_t>(layout.colWidth - static_cast<int>(cell.size())), ' ');
            } else if (static_cast<int>(cell.size()) > layout.colWidth) {
              cell.resize(static_cast<size_t>(layout.colWidth));
            }
            WORD attr = isSelected ? kAttrHighlight : (entry.isDir ? kAttrDir : kAttrNormal);
            screen.writeText(c * layout.colWidth, y, cell, attr);
          }
        }
      }

      int footerStart = listTop + listHeight;
      int line = footerStart;
      if (line < height) {
        line++;
      }
      std::string nowLabel = nowPlaying.empty() ? "(none)" : nowPlaying.filename().string();
      screen.writeText(0, line++, fitLine(std::string("  Now: ") + nowLabel, width), kAttrAccent);

      double currentSec = decoderReady ? static_cast<double>(state.framesPlayed.load()) / sampleRate : 0.0;
      double totalSec = (decoderReady && state.totalFrames > 0) ? static_cast<double>(state.totalFrames) / sampleRate : -1.0;
      std::string status = decoderReady ? (state.finished.load() ? "ended" : (state.paused.load() ? "paused" : "playing")) : "idle";
      std::string suffix = formatTime(currentSec) + " / " + formatTime(totalSec) + " " + status;
      int barWidth = width - static_cast<int>(suffix.size()) - 3;
      if (barWidth < 10) {
        suffix = formatTime(currentSec) + "/" + formatTime(totalSec);
        barWidth = width - static_cast<int>(suffix.size()) - 3;
      }
      if (barWidth < 10) {
        suffix = formatTime(currentSec);
        barWidth = width - static_cast<int>(suffix.size()) - 3;
      }
      if (barWidth < 5) {
        suffix.clear();
        barWidth = width - 2;
      }
      int maxBar = std::max(5, width - 2);
      barWidth = std::clamp(barWidth, 5, maxBar);
      int filled = 0;
      if (totalSec > 0.0 && std::isfinite(totalSec)) {
        double ratio = std::clamp(currentSec / totalSec, 0.0, 1.0);
        filled = static_cast<int>(std::round(ratio * barWidth));
      }
      progressBarX = 1;
      progressBarY = line;
      progressBarWidth = barWidth;

      screen.writeText(0, line, "[", kAttrProgressFrame);
      screen.writeRun(1, line, barWidth, ' ', kAttrProgressEmpty);
      if (filled > 0) {
        screen.writeRun(1, line, std::min(filled, barWidth), ' ', kAttrProgressFill);
      }
      screen.writeText(1 + barWidth, line, "]", kAttrProgressFrame);
      if (!suffix.empty()) {
        screen.writeText(2 + barWidth, line, " " + suffix, kAttrNormal);
      }

      screen.draw();
      lastDraw = now;
      dirty = false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  input.restore();
  screen.restore();
  std::cout << "\n";
  if (deviceReady) {
    ma_device_uninit(&state.device);
  }
  if (decoderReady) {
    ma_decoder_uninit(&state.decoder);
  }
  return 0;
}
