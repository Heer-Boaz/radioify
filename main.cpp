#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "asciiart.h"
#include "audioplayback.h"
#include "browsermeta.h"
#include "consoleinput.h"
#include "consolescreen.h"
#include "m4adecoder.h"
#include "miniaudio.h"
#include "radio.h"
#include "ui_helpers.h"
#include "videoplayback.h"

#ifndef RADIOIFY_ENABLE_TIMING_LOG
#define RADIOIFY_ENABLE_TIMING_LOG 0
#endif
#ifndef RADIOIFY_ENABLE_VIDEO_ERROR_LOG
#define RADIOIFY_ENABLE_VIDEO_ERROR_LOG 1
#endif

struct Options {
  std::string input;
  std::string output;
  // int bwHz = 4800;
  int bwHz = 5500;
  double noise = 0.012;  // tuned for a "modern recording through 1938 AM"
  // double noise = 0.006; // tuned for a "modern recording through 1938 AM"
  bool mono = true;
  bool play = true;
  bool dry = false;
  bool enableAscii = true;
  bool enableAudio = true;
  bool enableRadio = true;
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

static void showUsage(const char* exe) {
  std::string name = exe ? std::string(exe) : "radioify";
  logLine("Usage: " + name + " [options] [file_or_folder]");
  logLine("Options:");
  logLine("  --no-ascii   Disable ASCII video rendering");
  logLine("  --no-audio   Disable audio playback");
  logLine("  --no-radio   Start with radio filter disabled");
  logLine("  -h, --help   Show this help");
}

static Options parseArgs(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      showUsage(argv[0]);
      std::exit(0);
    }
    if (arg == "--no-ascii") {
      o.enableAscii = false;
      continue;
    }
    if (arg == "--no-audio") {
      o.enableAudio = false;
      continue;
    }
    if (arg == "--no-radio") {
      o.enableRadio = false;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      die("Unknown option: " + arg);
    }
    if (!o.input.empty()) {
      die("Provide a single file or folder path only.");
    }
    o.input = arg;
  }
  return o;
}

static bool isSupportedImageExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp";
}

static bool isVideoExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".mp4" || ext == ".webm";
}

static bool isSupportedAudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".m4a" ||
         ext == ".webm" || ext == ".mp4";
}

static bool isMiniaudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

static bool isSupportedMediaExt(const std::filesystem::path& p) {
  return isSupportedAudioExt(p) || isSupportedImageExt(p);
}

static bool isM4aExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".m4a" || ext == ".mp4" || ext == ".webm";
}

static void validateInputFile(const std::filesystem::path& p) {
  if (p.empty()) die("Missing input file path.");
  if (!std::filesystem::exists(p)) die("Input file not found: " + p.string());
  if (std::filesystem::is_directory(p))
    die("Input path must be a file: " + p.string());
  if (!isSupportedAudioExt(p)) {
    die("Unsupported input format '" + p.extension().string() +
        "'. Supported: .wav, .mp3, .flac, .m4a, .webm, .mp4.");
  }
}

static std::vector<FileEntry> listEntries(const std::filesystem::path& dir) {
  std::vector<FileEntry> entries;
  std::vector<FileEntry> items;

#ifdef _WIN32
  if (dir.empty() || dir == dir.root_path()) {
    for (const auto& drive : listDriveEntries()) {
      entries.push_back(FileEntry{drive.label, drive.path, true});
    }
  }
#endif

  if (dir.has_parent_path() && dir != dir.root_path()) {
    entries.push_back(FileEntry{"..", dir.parent_path(), true});
  }

  try {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
      const auto& p = entry.path();
      if (entry.is_directory()) {
        items.push_back(FileEntry{toUtf8String(p.filename()), p, true});
      } else if (entry.is_regular_file() && isSupportedMediaExt(p)) {
        items.push_back(FileEntry{toUtf8String(p.filename()), p, false});
      }
    }
  } catch (...) {
    return entries;
  }

  std::sort(items.begin(), items.end(),
            [](const FileEntry& a, const FileEntry& b) {
              if (a.isDir != b.isDir) return a.isDir > b.isDir;
              return toLower(a.name) < toLower(b.name);
            });

  entries.insert(entries.end(), items.begin(), items.end());
  return entries;
}

static void refreshBrowser(BrowserState& state,
                           const std::string& initialName) {
  state.entries = listEntries(state.dir);
  if (state.entries.empty()) {
    state.selected = 0;
    state.scrollRow = 0;
    return;
  }

  if (state.selected < 0 ||
      state.selected >= static_cast<int>(state.entries.size())) {
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

static bool showAsciiArt(const std::filesystem::path& file, ConsoleInput& input,
                         ConsoleScreen& screen, const Style& baseStyle,
                         const Style& accentStyle, const Style& dimStyle) {
  AsciiArt art;
  std::string error;
  bool ok = false;

  auto renderFrame = [&]() {
    screen.updateSize();
    int width = std::max(20, screen.width());
    int height = std::max(10, screen.height());
    const int headerLines = 2;
    const int footerLines = 1;
    int artTop = headerLines;
    int maxHeight = std::max(1, height - headerLines - footerLines);

    error.clear();
    ok = renderAsciiArt(file, width, maxHeight, art, &error);

    screen.clear(baseStyle);
    std::string title = "Preview: " + toUtf8String(file.filename());
    screen.writeText(0, 0, fitLine(title, width), accentStyle);
    screen.writeText(0, 1, fitLine("Press any key to return", width), dimStyle);

    if (!ok) {
      screen.writeText(0, artTop, fitLine("Failed to open image.", width),
                       dimStyle);
      if (!error.empty() && artTop + 1 < height) {
        screen.writeText(0, artTop + 1, fitLine(error, width), dimStyle);
      }
      screen.draw();
      return;
    }

    int artWidth = std::min(art.width, width);
    int artHeight = std::min(art.height, maxHeight);
    int artX = std::max(0, (width - artWidth) / 2);

    for (int y = 0; y < artHeight; ++y) {
      for (int x = 0; x < artWidth; ++x) {
        const auto& cell = art.cells[static_cast<size_t>(y * art.width + x)];
        Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
        screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
      }
    }
    screen.draw();
  };

  renderFrame();

  InputEvent ev{};
  while (true) {
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Resize) {
        renderFrame();
        continue;
      }
      if (ev.type == InputEvent::Type::Key) {
        return ok;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

static void renderToFile(const Options& o, const Radio1938& radio1938Template,
                         bool useRadio1938) {
  const uint32_t sampleRate = 48000;
  const uint32_t channels = useRadio1938 ? 1 : (o.mono ? 1 : 2);
  const bool useM4a = isM4aExt(o.input);
  ma_decoder decoder{};
  M4aDecoder m4a{};
  if (useM4a) {
    std::string error;
    if (!m4a.init(o.input, channels, sampleRate, &error)) {
      die(error.empty() ? "Failed to open input for decoding." : error);
    }
  } else {
    ma_decoder_config decConfig =
        ma_decoder_config_init(ma_format_f32, channels, sampleRate);
    if (ma_decoder_init_file(o.input.c_str(), &decConfig, &decoder) !=
        MA_SUCCESS) {
      die("Failed to open input for decoding.");
    }
  }

  ma_encoder encoder{};
  ma_encoder_config encConfig = ma_encoder_config_init(
      ma_encoding_format_wav, ma_format_s16, channels, sampleRate);
  if (ma_encoder_init_file(o.output.c_str(), &encConfig, &encoder) !=
      MA_SUCCESS) {
    if (useM4a) {
      m4a.uninit();
    } else {
      ma_decoder_uninit(&decoder);
    }
    die("Failed to open output for encoding.");
  }

  Radio1938 radio1938 = radio1938Template;
  radio1938.init(channels, static_cast<float>(sampleRate),
                 static_cast<float>(o.bwHz), static_cast<float>(o.noise));
  constexpr uint32_t chunkFrames = 1024;
  std::vector<float> buffer(chunkFrames * channels);
  std::vector<int16_t> out(buffer.size());

  while (true) {
    uint64_t framesRead = 0;
    if (useM4a) {
      if (!m4a.readFrames(buffer.data(), chunkFrames, &framesRead)) {
        ma_encoder_uninit(&encoder);
        m4a.uninit();
        die("Failed to decode input.");
      }
      if (framesRead == 0) break;
    } else {
      ma_uint64 framesReadMa = 0;
      ma_result res = ma_decoder_read_pcm_frames(&decoder, buffer.data(),
                                                 chunkFrames, &framesReadMa);
      framesRead = framesReadMa;
      if (framesRead == 0 || res == MA_AT_END) break;
    }

    if (!o.dry && useRadio1938) {
      radio1938.process(buffer.data(), static_cast<uint32_t>(framesRead));
    }

    for (size_t i = 0; i < static_cast<size_t>(framesRead * channels); ++i) {
      float v = std::clamp(buffer[i], -1.0f, 1.0f);
      out[i] = static_cast<int16_t>(std::lrint(v * 32767.0f));
    }

    ma_uint64 framesWritten = 0;
    ma_encoder_write_pcm_frames(&encoder, out.data(),
                                static_cast<ma_uint64>(framesRead),
                                &framesWritten);
  }

  ma_encoder_uninit(&encoder);
  if (useM4a) {
    m4a.uninit();
  } else {
    ma_decoder_uninit(&decoder);
  }
}

int main(int argc, char** argv) {
  Options o = parseArgs(argc, argv);
  configureFfmpegVideoLog({});

  float lpHz = static_cast<float>(o.bwHz);
  const uint32_t sampleRate = 48000;
  const uint32_t baseChannels = o.mono ? 1 : 2;
  uint32_t channels = baseChannels;

  Radio1938 radio1938Template;
  radio1938Template.init(channels, static_cast<float>(sampleRate), lpHz,
                         static_cast<float>(o.noise));

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
        initialName = toUtf8String(inputPath.filename());
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

  AudioPlaybackConfig audioConfig;
  audioConfig.enableAudio = o.enableAudio;
  audioConfig.enableRadio = o.enableRadio;
  audioConfig.mono = o.mono;
  audioConfig.dry = o.dry;
  audioConfig.bwHz = o.bwHz;
  audioConfig.noise = o.noise;
  audioInit(audioConfig);

  VideoPlaybackConfig videoConfig;
  videoConfig.enableAscii = o.enableAscii;
  videoConfig.enableAudio = o.enableAudio;

  std::filesystem::path pendingImage;
  bool hasPendingImage = false;
  std::filesystem::path pendingVideo;
  bool hasPendingVideo = false;

  if (!o.input.empty() && o.play && std::filesystem::exists(o.input)) {
    std::filesystem::path inputPath(o.input);
    if (!std::filesystem::is_directory(inputPath)) {
      if (isSupportedImageExt(inputPath)) {
        pendingImage = inputPath;
        hasPendingImage = true;
      } else if (isVideoExt(inputPath)) {
        pendingVideo = inputPath;
        hasPendingVideo = true;
      } else {
        audioStartFile(inputPath);
      }
    }
  }

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
    renderToFile(renderOpt, radio1938Template, audioIsRadioEnabled());
    logLine("Done.");
  };

  const Color kBgBase{12, 15, 20};
  const Style kStyleNormal{{215, 220, 226}, kBgBase};
  const Style kStyleHeader{{230, 238, 248}, {18, 28, 44}};
  const Style kStyleHeaderGlow{{255, 213, 118}, {22, 34, 52}};
  const Style kStyleHeaderHot{{255, 249, 214}, {38, 50, 72}};
  const Style kStyleAccent{{255, 214, 120}, kBgBase};
  const Style kStyleDim{{138, 144, 153}, kBgBase};
  const Style kStyleDir{{110, 231, 183}, kBgBase};
  const Style kStyleHighlight{{15, 20, 28}, {230, 238, 248}};
  const Style kStyleBreadcrumbHover{{15, 20, 28}, {255, 214, 120}};
  const Color kProgressStart{110, 231, 183};
  const Color kProgressEnd{255, 214, 110};
  const Style kStyleProgressEmpty{{32, 38, 46}, {32, 38, 46}};
  const Style kStyleProgressFrame{{160, 170, 182}, kBgBase};

  screen.clear(kStyleNormal);
  screen.draw();

  bool running = true;
  bool dirty = true;
  auto lastDraw = std::chrono::steady_clock::now();
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
  int breadcrumbHover = -1;
  const int headerLines = 5;
  const int listTop = headerLines + 1;
  const int breadcrumbY = listTop - 1;
  const int footerLines = 4;
  int width = 0;
  int height = 0;
  int listHeight = 0;
  GridLayout layout;
  BreadcrumbLine breadcrumbLine;
  bool rendered = false;

  if (hasPendingImage) {
    showAsciiArt(pendingImage, input, screen, kStyleNormal, kStyleAccent,
                 kStyleDim);
    dirty = true;
  }
  if (hasPendingVideo) {
    bool handled =
        showAsciiVideo(pendingVideo, input, screen, kStyleNormal, kStyleAccent,
                       kStyleDim, kStyleProgressEmpty, kStyleProgressFrame,
                       kProgressStart, kProgressEnd, videoConfig);
    if (!handled) {
      audioStartFile(pendingVideo);
    }
    dirty = true;
  }

  auto rebuildLayout = [&]() {
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
    breadcrumbLine = buildBreadcrumbLine(browser.dir, width);
    if (breadcrumbHover >= static_cast<int>(breadcrumbLine.crumbs.size())) {
      breadcrumbHover = -1;
    }
  };

  InputCallbacks callbacks;
  callbacks.onRefreshBrowser = [&](BrowserState& nextBrowser,
                                   const std::string& initialName) {
    refreshBrowser(nextBrowser, initialName);
  };
  callbacks.onPlayFile = [&](const std::filesystem::path& file) {
    if (isSupportedImageExt(file)) {
      showAsciiArt(file, input, screen, kStyleNormal, kStyleAccent, kStyleDim);
      return true;
    }
    if (isVideoExt(file)) {
      bool handled = showAsciiVideo(
          file, input, screen, kStyleNormal, kStyleAccent, kStyleDim,
          kStyleProgressEmpty, kStyleProgressFrame, kProgressStart,
          kProgressEnd, videoConfig);
      if (handled) return true;
    }
    return audioStartFile(file);
  };
  callbacks.onRenderFile = [&](const std::filesystem::path& file) {
    renderFile(file);
    rendered = true;
  };
  callbacks.onTogglePause = [&]() {
    audioTogglePause();
  };
  callbacks.onToggleRadio = [&]() {
    audioToggleRadio();
  };
  callbacks.onSeekBy = [&](int direction) { audioSeekBy(direction); };
  callbacks.onSeekToRatio = [&](double ratio) { audioSeekToRatio(ratio); };
  callbacks.onResize = [&]() { rebuildLayout(); };

  while (running) {
    rebuildLayout();

    InputEvent ev{};
    while (input.poll(ev)) {
      handleInputEvent(ev, browser, layout, breadcrumbLine, breadcrumbY,
                       listTop, progressBarX, progressBarY, progressBarWidth,
                       o.play, audioIsReady(), breadcrumbHover, dirty, running,
                       callbacks);
      if (rendered) {
        audioShutdown();
        return 0;
      }
      if (!running) break;
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

      screen.clear(kStyleNormal);

      const std::string titleRaw = "Radioify";
      std::string title = titleRaw;
      if (static_cast<int>(title.size()) > width) {
        title = fitLine(titleRaw, width);
      }
      int titleLen = static_cast<int>(title.size());
      int titleX = std::max(0, (width - titleLen) / 2);
      double seconds =
          std::chrono::duration<double>(now.time_since_epoch()).count();
      double speed = 1.3;
      float pulse = static_cast<float>(0.5 * (std::sin(seconds * speed) + 1.0));
      pulse = clamp01(pulse);
      pulse = pulse * pulse * (3.0f - 2.0f * pulse);
      float t = std::pow(pulse, 0.6f);
      float flash = 0.0f;
      if (t > 0.88f) {
        flash = (t - 0.88f) / 0.12f;
        flash = flash * flash;
      }
      Color headerBg = lerpColor(kStyleHeader.bg, kStyleHeaderHot.bg,
                                 std::min(0.85f, t * 0.9f));
      headerBg = lerpColor(headerBg, Color{52, 44, 26}, flash * 0.7f);
      Style headerLineStyle{kStyleHeader.fg, headerBg};
      screen.writeRun(0, 0, width, L' ', headerLineStyle);

      Color titleFg;
      if (t < 0.35f) {
        titleFg = lerpColor(kStyleHeader.fg, kStyleHeaderGlow.fg, t / 0.35f);
      } else {
        float hotT = (t - 0.35f) / 0.65f;
        titleFg =
            lerpColor(kStyleHeaderGlow.fg, kStyleHeaderHot.fg, clamp01(hotT));
      }
      if (t > 0.85f) {
        float whiteT = (t - 0.85f) / 0.15f;
        titleFg = lerpColor(titleFg, Color{255, 255, 255}, clamp01(whiteT));
      }
      if (flash > 0.0f) {
        titleFg = lerpColor(titleFg, Color{255, 236, 186}, clamp01(flash));
      }
      Style titleAttr{titleFg, headerBg};
      for (int i = 0; i < titleLen; ++i) {
        wchar_t ch = static_cast<wchar_t>(
            static_cast<unsigned char>(title[static_cast<size_t>(i)]));
        screen.writeChar(titleX + i, 0, ch, titleAttr);
      }
      breadcrumbLine = buildBreadcrumbLine(browser.dir, width);
      if (breadcrumbHover >= static_cast<int>(breadcrumbLine.crumbs.size())) {
        breadcrumbHover = -1;
      }
      screen.writeText(0, breadcrumbY, breadcrumbLine.text, kStyleAccent);
      if (breadcrumbHover >= 0) {
        const auto& crumb =
            breadcrumbLine.crumbs[static_cast<size_t>(breadcrumbHover)];
        std::string hoverText = utf8Slice(breadcrumbLine.text, crumb.startX,
                                          crumb.endX - crumb.startX);
        screen.writeText(crumb.startX, breadcrumbY, hoverText,
                         kStyleBreadcrumbHover);
      }
      if (o.play) {
        screen.writeText(
            0, 2,
            fitLine("  Mouse=select  Click=play/enter  Backspace=up  "
                    "Click+drag bar=seek  Space=pause  Arrows=move  "
                    "PgUp/PgDn=page  "
                    "Ctrl+Left/Right=seek  R=toggle  Q=quit",
                    width),
            kStyleNormal);
      } else {
        screen.writeText(
            0, 2,
            fitLine("  Mouse=select  Click=render/enter  Backspace=up  "
                    "Arrows=move  PgUp/PgDn=page  R=toggle  Q=quit",
                    width),
            kStyleNormal);
      }
      std::string filterLabel =
          audioIsRadioEnabled() ? "1938 radio" : "dry";
      screen.writeText(0, 3,
                       fitLine(std::string("  Filter: ") + filterLabel, width),
                       kStyleDim);
      screen.writeText(
          0, 4,
          fitLine("  Showing: folders + "
                  ".wav/.mp3/.flac/.m4a/.webm/.mp4/.jpg/.jpeg/.png/.bmp",
                  width),
          kStyleDim);

      drawBrowserEntries(screen, browser, layout, listTop, listHeight,
                         kStyleNormal, kStyleNormal, kStyleDir, kStyleHighlight,
                         kStyleDim, isSupportedImageExt, isVideoExt,
                         isSupportedAudioExt);

      int footerStart = listTop + listHeight;
      int line = footerStart;
      if (line < height) {
        line++;
      }
      if (line < height) {
        std::string meta = buildSelectionMeta(browser, isVideoExt);
        if (!meta.empty()) {
          screen.writeText(0, line++, fitLine(meta, width), kStyleDim);
        }
      }
      std::filesystem::path nowPlaying = audioGetNowPlaying();
      std::string nowLabel =
          nowPlaying.empty() ? "(none)" : toUtf8String(nowPlaying.filename());
      screen.writeText(0, line++, fitLine(std::string(" ") + nowLabel, width),
                       kStyleAccent);

      bool audioReady = audioIsReady();
      double currentSec = audioReady ? audioGetTimeSec() : 0.0;
      double totalSec = audioReady ? audioGetTotalSec() : -1.0;
      double displaySec = currentSec;
      if (audioReady && audioIsSeeking()) {
        double seekSec = audioGetSeekTargetSec();
        if (seekSec >= 0.0) {
          displaySec = seekSec;
        }
      }
      std::string status;
      if (audioReady) {
        if (audioIsFinished()) {
          status = "\xE2\x96\xA0";  // ended icon
        } else if (audioIsPaused()) {
          status = "\xE2\x8F\xB8";  // paused icon
        } else {
          status = "\xE2\x96\xB6";  // playing icon
        }
      } else {
        status = "\xE2\x97\x8B";  // idle icon
      }
      std::string suffix =
          formatTime(displaySec) + " / " + formatTime(totalSec) + " " + status;
      int suffixWidth = utf8CodepointCount(suffix);
      int barWidth = width - suffixWidth - 3;
      if (barWidth < 10) {
        suffix = formatTime(displaySec) + "/" + formatTime(totalSec);
        suffixWidth = utf8CodepointCount(suffix);
        barWidth = width - suffixWidth - 3;
      }
      if (barWidth < 10) {
        suffix = formatTime(displaySec);
        suffixWidth = utf8CodepointCount(suffix);
        barWidth = width - suffixWidth - 3;
      }
      if (barWidth < 5) {
        suffix.clear();
        barWidth = width - 2;
      }
      int maxBar = std::max(5, width - 2);
      barWidth = std::clamp(barWidth, 5, maxBar);
      double ratio = 0.0;
      if (totalSec > 0.0 && std::isfinite(totalSec)) {
        ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
      }
      progressBarX = 1;
      progressBarY = line;
      progressBarWidth = barWidth;

      screen.writeChar(0, line, L'|', kStyleProgressFrame);
      auto barCells = renderProgressBarCells(
          ratio, barWidth, kStyleProgressEmpty, kProgressStart, kProgressEnd);
      for (int i = 0; i < barWidth; ++i) {
        const auto& cell = barCells[static_cast<size_t>(i)];
        screen.writeChar(1 + i, line, cell.ch, cell.style);
      }
      screen.writeChar(1 + barWidth, line, L'|', kStyleProgressFrame);
      if (!suffix.empty()) {
        screen.writeText(2 + barWidth, line, " " + suffix, kStyleNormal);
      }

      screen.draw();
      lastDraw = now;
      dirty = false;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Clear screen before exiting
  screen.clear(kStyleNormal);
  screen.draw();
  input.restore();
  screen.restore();
  std::cout << "\n";
  audioShutdown();
  return 0;
}
