#include "audioplayback.h"

#include "app_common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
#include <emu2212/emu2212.h>
}

#include "clock.h"
#include "ffmpegaudio.h"
#include "flacaudio.h"
#include "gmeaudio.h"
#include "gsfaudio.h"
#include "vgmaudio.h"
#include "kssaudio.h"
#include "melodyanalysiscache.h"
#include "midiaudio.h"
#include "psfaudio.h"
#include "radio.h"
#include "radiopreview.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define MINIAUDIO_IMPLEMENTATION
#define MA_ENABLE_WAV
#define MA_ENABLE_MP3
#define MA_ENABLE_FLAC
#include "miniaudio.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "timing_log.h"

namespace {
constexpr uint32_t kMsxClockHz = 3579545u;
constexpr float kAuditionGain = 0.28f;
constexpr int kVgmSpeedStepCount = 6;
constexpr int kVgmVolumeStepCount = 6;
constexpr int kVgmLoopSteps[] = {0, 1, 2, 3, 4, 6, 8, 12, 16};
constexpr int kVgmFadeStepsMs[] = {0, 1000, 2000, 3000, 5000, 8000, 10000, 15000};
constexpr int kVgmEndSilenceStepsMs[] = {0, 250, 500, 1000, 2000, 3000, 5000};
constexpr uint32_t kVgmSampleRateSteps[] = {0, 11025, 22050, 32000, 44100, 48000, 88200, 96000};
std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static std::string toUtf8String(const std::filesystem::path& p) {
#ifdef _WIN32
  auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
#else
  return p.string();
#endif
}

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int advanceIndex(int idx, int count, int direction) {
  if (count <= 0) return 0;
  if (direction > 0) {
    idx++;
    if (idx >= count) idx = 0;
  } else {
    idx--;
    if (idx < 0) idx = count - 1;
  }
  return idx;
}

template <typename T>
int findIndex(const T* values, size_t count, T value) {
  for (size_t i = 0; i < count; ++i) {
    if (values[i] == value) return static_cast<int>(i);
  }
  return 0;
}

bool isSupportedAudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".m4a" ||
         ext == ".webm" || ext == ".mp4" || ext == ".mov" || ext == ".ogg" ||
         ext == ".kss" ||
         ext == ".nsf" ||
#if !RADIOIFY_DISABLE_GSF_GPL
         ext == ".gsf" || ext == ".minigsf" ||
#endif
         ext == ".mid" || ext == ".midi" ||
         ext == ".vgm" || ext == ".vgz" || ext == ".psf" ||
         ext == ".minipsf" ||
         ext == ".psf2" || ext == ".minipsf2";
}

bool isMiniaudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3";
}

bool isFlacExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".flac";
}

bool isGmeExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".nsf";
}

bool isMidiExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".mid" || ext == ".midi";
}

bool isGsfExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
#if !RADIOIFY_DISABLE_GSF_GPL
  return ext == ".gsf" || ext == ".minigsf";
#else
  (void)ext;
  return false;
#endif
}

bool isVgmExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".vgm" || ext == ".vgz";
}

bool isM4aExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".m4a" || ext == ".mp4" || ext == ".webm" || ext == ".mov";
}

bool isKssExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".kss";
}

bool isOggExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".ogg";
}

bool isPsfExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".psf" || ext == ".minipsf" || ext == ".psf2" ||
         ext == ".minipsf2";
}

void validateInputFile(const std::filesystem::path& p) {
  if (p.empty()) return;
  if (!std::filesystem::exists(p)) return;
  if (std::filesystem::is_directory(p)) return;
  if (!isSupportedAudioExt(p)) return;
}

enum class AudioMode : int {
  None = 0,
  Stream,
  M4a,
  Ffmpeg,
  Flac,
  Miniaudio,
  Gme,
  Midi,
  Gsf,
  Vgm,
  Kss,
  Psf,
};

using BackendInitProc = bool (*)(const std::filesystem::path& file,
                                 uint64_t startFrame,
                                 int trackIndex,
                                 std::string* error);
using BackendUninitProc = void (*)();
using BackendReadProc = bool (*)(float* out, uint32_t frameCount,
                                 uint64_t* framesRead);
using BackendSeekProc = bool (*)(uint64_t frame);
using BackendTotalFramesProc = bool (*)(uint64_t* outFrames);
using BackendWarningProc = std::string (*)();

struct AudioBackendHandlers {
  AudioMode mode = AudioMode::None;
  bool supportsTrackIndex = false;
  bool allowSeekInCallback = true;
  bool finishOnShortRead = true;
  bool allowConcurrentOfflineAnalysis = true;
  BackendInitProc init = nullptr;
  BackendUninitProc uninit = nullptr;
  BackendReadProc read = nullptr;
  BackendSeekProc seek = nullptr;
  BackendTotalFramesProc totalFrames = nullptr;
  BackendWarningProc warning = nullptr;
};

struct AudioRingBuffer {
  std::vector<float> data;
  size_t capacityFrames = 0;
  size_t readPos = 0;
  size_t writePos = 0;
  size_t bufferedFrames = 0;
};

struct AudioSampleRing {
  std::vector<float> buf;
  uint32_t channels = 0;
  uint64_t capFrames = 0;
  std::atomic<uint64_t> rpos{0};
  std::atomic<uint64_t> wpos{0};

  void init(uint64_t capacityFrames, uint32_t ch) {
    channels = ch;
    capFrames = capacityFrames;
    buf.assign(capFrames * channels, 0.0f);
    rpos.store(0, std::memory_order_relaxed);
    wpos.store(0, std::memory_order_relaxed);
  }

  void reset() {
    rpos.store(0, std::memory_order_release);
    wpos.store(0, std::memory_order_release);
  }

  uint64_t bufferedFrames() const {
    uint64_t w = wpos.load(std::memory_order_acquire);
    uint64_t r = rpos.load(std::memory_order_acquire);
    return w - r;
  }

  uint64_t writableFrames() const {
    return capFrames - bufferedFrames();
  }

  uint64_t writeSome(const float* in, uint64_t frames) {
    uint64_t w = wpos.load(std::memory_order_relaxed);
    uint64_t r = rpos.load(std::memory_order_acquire);
    uint64_t avail = capFrames - (w - r);
    if (avail == 0) return 0;
    uint64_t n = (frames < avail) ? frames : avail;

    uint64_t wi = w % capFrames;
    uint64_t first = std::min<uint64_t>(n, capFrames - wi);

    std::memcpy(buf.data() + wi * channels, in,
                first * channels * sizeof(float));
    if (n > first) {
      std::memcpy(buf.data(), in + first * channels,
                  (n - first) * channels * sizeof(float));
    }

    wpos.store(w + n, std::memory_order_release);
    return n;
  }

  uint64_t readSome(float* out, uint64_t frames) {
    uint64_t r = rpos.load(std::memory_order_relaxed);
    uint64_t w = wpos.load(std::memory_order_acquire);
    uint64_t avail = w - r;
    if (avail == 0) return 0;
    uint64_t n = (frames < avail) ? frames : avail;

    uint64_t ri = r % capFrames;
    uint64_t first = std::min<uint64_t>(n, capFrames - ri);

    std::memcpy(out, buf.data() + ri * channels,
                first * channels * sizeof(float));
    if (n > first) {
      std::memcpy(out + first * channels, buf.data(),
                  (n - first) * channels * sizeof(float));
    }

    rpos.store(r + n, std::memory_order_release);
    return n;
  }

  uint64_t dropSome(uint64_t frames) {
    uint64_t r = rpos.load(std::memory_order_relaxed);
    uint64_t w = wpos.load(std::memory_order_acquire);
    uint64_t avail = w - r;
    if (avail == 0) return 0;
    uint64_t n = (frames < avail) ? frames : avail;
    rpos.store(r + n, std::memory_order_release);
    return n;
  }
};

void audioRingBufferInit(AudioRingBuffer* buffer, size_t frames,
                         uint32_t channels) {
  if (!buffer) return;
  buffer->capacityFrames = frames;
  buffer->data.assign(frames * channels, 0.0f);
  buffer->readPos = 0;
  buffer->writePos = 0;
  buffer->bufferedFrames = 0;
}

void audioRingBufferReset(AudioRingBuffer* buffer) {
  if (!buffer) return;
  buffer->readPos = 0;
  buffer->writePos = 0;
  buffer->bufferedFrames = 0;
}

size_t audioRingBufferSpace(const AudioRingBuffer* buffer) {
  if (!buffer) return 0;
  if (buffer->capacityFrames < buffer->bufferedFrames) return 0;
  return buffer->capacityFrames - buffer->bufferedFrames;
}

size_t audioRingBufferSize(const AudioRingBuffer* buffer) {
  if (!buffer) return 0;
  return buffer->bufferedFrames;
}

size_t audioRingBufferRead(AudioRingBuffer* buffer, float* out, size_t frames,
                           uint32_t channels) {
  if (!buffer || !out || buffer->capacityFrames == 0 || frames == 0) return 0;
  size_t toRead = std::min(frames, buffer->bufferedFrames);
  if (toRead == 0) return 0;
  size_t first = std::min(toRead, buffer->capacityFrames - buffer->readPos);
  size_t firstSamples = first * channels;
  std::memcpy(out, buffer->data.data() + buffer->readPos * channels,
              firstSamples * sizeof(float));
  if (toRead > first) {
    size_t second = toRead - first;
    std::memcpy(out + firstSamples, buffer->data.data(),
                second * channels * sizeof(float));
  }
  buffer->readPos = (buffer->readPos + toRead) % buffer->capacityFrames;
  buffer->bufferedFrames -= toRead;
  return toRead;
}

size_t audioRingBufferWrite(AudioRingBuffer* buffer, const float* in,
                            size_t frames, uint32_t channels) {
  if (!buffer || !in || buffer->capacityFrames == 0 || frames == 0) return 0;
  size_t spaceFrames = audioRingBufferSpace(buffer);
  size_t toWrite = std::min(frames, spaceFrames);
  if (toWrite == 0) return 0;
  size_t first = std::min(toWrite, buffer->capacityFrames - buffer->writePos);
  size_t firstSamples = first * channels;
  std::memcpy(buffer->data.data() + buffer->writePos * channels, in,
              firstSamples * sizeof(float));
  if (toWrite > first) {
    size_t second = toWrite - first;
    std::memcpy(buffer->data.data(), in + firstSamples,
                second * channels * sizeof(float));
  }
  buffer->writePos = (buffer->writePos + toWrite) % buffer->capacityFrames;
  buffer->bufferedFrames += toWrite;
  return toWrite;
}

uint64_t rescaleFrames(uint64_t frames, uint32_t inRate, uint32_t outRate) {
  if (frames == 0 || inRate == 0 || outRate == 0 || inRate == outRate) {
    return frames;
  }
  double scaled = static_cast<double>(frames) * static_cast<double>(outRate) /
                  static_cast<double>(inRate);
  return static_cast<uint64_t>(std::llround(scaled));
}

enum class AuditionKind {
  Psg,
  SccWave,
};

struct AuditionTone {
  AuditionKind kind = AuditionKind::SccWave;
  PSG* psg = nullptr;
  SCC* scc = nullptr;
  float gain = 0.0f;
};

float clampHz(float hz, uint32_t sampleRate) {
  float maxHz = static_cast<float>(sampleRate) * 0.45f;
  if (!std::isfinite(hz) || hz <= 0.0f) return 440.0f;
  if (hz < 20.0f) return 20.0f;
  if (hz > maxHz) return maxHz;
  return hz;
}

uint32_t fnv1a32(const uint8_t* data, size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

bool buildPsgAuditionTone(const KssInstrumentProfile& profile,
                          uint32_t sampleRate, KssPsgType psgType,
                          AuditionTone* out) {
  if (!out || profile.device != KssInstrumentDevice::Psg) return false;
  if (profile.data.size() < 5) return false;

  const uint8_t mix = profile.data[0];
  uint8_t volReg = profile.data[1];
  const uint8_t envLo = profile.data[2];
  const uint8_t envHi = profile.data[3];
  const uint8_t envShape = profile.data[4];
  uint8_t noisePeriod = 16;
  if (profile.data.size() >= 6) {
    noisePeriod = static_cast<uint8_t>(profile.data[5] & 0x1f);
    if (noisePeriod == 0) noisePeriod = 1;
  }
  bool useEnv = (volReg & 0x10) != 0;
  uint8_t volume = volReg & 0x0f;
  if (useEnv && envLo == 0 && envHi == 0) {
    useEnv = false;
  }
  if (!useEnv) {
    if (volume == 0) volume = 0x0f;
    volReg = volume;
  }

  PSG* psg = PSG_new(kMsxClockHz, sampleRate);
  if (!psg) return false;
  PSG_reset(psg);
  PSG_setClockDivider(psg, 1);
  switch (psgType) {
    case KssPsgType::Ay:
      PSG_setVolumeMode(psg, 2);
      break;
    case KssPsgType::Ym:
      PSG_setVolumeMode(psg, 1);
      break;
    case KssPsgType::Auto:
    default:
      PSG_setVolumeMode(psg, 0);
      break;
  }

  float freqHz = clampHz(440.0f, sampleRate);
  uint16_t period =
      static_cast<uint16_t>(std::llround(static_cast<double>(kMsxClockHz) /
                                         (16.0 * freqHz)));
  if (period == 0) period = 1;

  const int channel = 0;
  PSG_writeReg(psg, 0, static_cast<uint8_t>(period & 0xff));
  PSG_writeReg(psg, 1, static_cast<uint8_t>((period >> 8) & 0x0f));
  PSG_writeReg(psg, 6, noisePeriod);

  uint8_t mixer = 0x3f;
  if ((mix & 0x01) == 0) mixer &= ~(1 << channel);
  if ((mix & 0x02) == 0) mixer &= ~(1 << (channel + 3));
  PSG_writeReg(psg, 7, mixer);

  PSG_writeReg(psg, 8, useEnv ? static_cast<uint8_t>(0x10 | volume) : volReg);
  PSG_writeReg(psg, 9, 0);
  PSG_writeReg(psg, 10, 0);
  if (useEnv) {
    PSG_writeReg(psg, 11, envLo);
    PSG_writeReg(psg, 12, envHi);
    PSG_writeReg(psg, 13, envShape);
  } else {
    PSG_writeReg(psg, 11, 0);
    PSG_writeReg(psg, 12, 0);
    PSG_writeReg(psg, 13, 0);
  }

  out->kind = AuditionKind::Psg;
  out->psg = psg;
  out->scc = nullptr;
  out->gain = kAuditionGain;
  return true;
}

bool buildSccAuditionTone(const KssInstrumentProfile& profile,
                          uint32_t sampleRate, KssSccType sccType,
                          KssQuality sccQuality, AuditionTone* out) {
  if (!out || profile.device != KssInstrumentDevice::Scc) return false;
  if (profile.data.size() < 32) return false;

  SCC* scc = SCC_new(kMsxClockHz, sampleRate);
  if (!scc) return false;
  SCC_reset(scc);
  SCC_set_rate(scc, sampleRate);
  SCC_set_quality(scc, sccQuality == KssQuality::High ? 1u : 0u);
  switch (sccType) {
    case KssSccType::Standard:
      SCC_set_type(scc, SCC_STANDARD);
      break;
    case KssSccType::Enhanced:
      SCC_set_type(scc, SCC_ENHANCED);
      break;
    case KssSccType::Auto:
    default:
      SCC_set_type(scc, SCC_ENHANCED);
      break;
  }

  for (size_t i = 0; i < 32; ++i) {
    SCC_writeReg(scc, static_cast<uint32_t>(i), profile.data[i]);
  }

  float freqHz = clampHz(440.0f, sampleRate);
  double raw = static_cast<double>(kMsxClockHz) / (32.0 * freqHz) - 1.0;
  uint32_t freqReg = raw <= 0.0 ? 0 : static_cast<uint32_t>(std::llround(raw));
  freqReg = std::min<uint32_t>(freqReg, 0xFFFu);
  if (freqReg < 9) freqReg = 9;
  SCC_writeReg(scc, 0xC0, freqReg & 0xFF);
  SCC_writeReg(scc, 0xC1, (freqReg >> 8) & 0x0F);

  uint8_t volume = profile.volume & 0x0F;
  if (volume == 0) volume = 0x0F;
  SCC_writeReg(scc, 0xD0, volume);
  SCC_writeReg(scc, 0xE1, 0x01);

  out->kind = AuditionKind::SccWave;
  out->psg = nullptr;
  out->scc = scc;
  out->gain = kAuditionGain;
  return true;
}

float renderAuditionSample(AuditionTone& tone) {
  if (tone.kind == AuditionKind::Psg) {
    if (!tone.psg) return 0.0f;
    int16_t sample = PSG_calc(tone.psg);
    return (static_cast<float>(sample) / 32768.0f) * tone.gain;
  }
  if (!tone.scc) return 0.0f;
  int16_t sample = SCC_calc(tone.scc);
  return (static_cast<float>(sample) / 32768.0f) * tone.gain;
}

struct AudioMetadata {
  uint64_t wpos;      // The write position (wpos) when this packet was added
  int64_t ptsUs;      // The PTS of the first sample in this packet
  int serial;         // The serial of this packet
};

struct AudioState {
  ma_decoder decoder{};
  FfmpegAudioDecoder ffmpeg{};
  FlacAudioDecoder flac{};
  GmeAudioDecoder gme{};
  GsfAudioDecoder gsf{};
  VgmAudioDecoder vgm{};
  KssAudioDecoder kss{};
  PsfAudioDecoder psf{};
  MidiAudioDecoder midi{};
  ma_device device{};
  Radio1938 radio1938{};
  AudioRingBuffer m4aBuffer;
  std::mutex m4aMutex;
  std::condition_variable m4aCv;
  std::thread m4aThread;
  bool m4aInitDone = false;
  bool m4aInitOk = false;
  std::string m4aInitError;
  std::atomic<bool> m4aThreadRunning{false};
  std::atomic<bool> m4aStop{false};
  std::atomic<bool> m4aAtEnd{false};
  std::thread decodeThread;
  std::atomic<bool> decodeThreadRunning{false};
  std::atomic<bool> decodeStop{false};
  std::mutex decodeMutex;
  std::condition_variable decodeCv;
  std::atomic<bool> externalStream{false};
  std::atomic<bool> paused{false};
  std::atomic<bool> hold{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> useRadio1938{true};
  std::atomic<AudioMode> mode{AudioMode::None};
  const AudioBackendHandlers* backend = nullptr;
  std::atomic<bool> seekRequested{false};
  std::atomic<int64_t> pendingSeekFrames{0};
  std::atomic<uint64_t> framesPlayed{0};
  std::atomic<uint64_t> callbackCount{0};
  std::atomic<uint64_t> framesRequested{0};
  std::atomic<uint64_t> framesReadTotal{0};
  std::atomic<uint64_t> shortReadCount{0};
  std::atomic<uint64_t> silentFrames{0};
  std::atomic<uint64_t> pausedCallbacks{0};
  std::atomic<uint32_t> lastCallbackFrames{0};
  std::atomic<uint32_t> lastFramesRead{0};
  std::atomic<uint64_t> audioClockFrames{0};
  std::atomic<uint64_t> totalFrames{0};
  std::atomic<uint64_t> audioPaddingFrames{0};
  std::atomic<uint64_t> audioTrailingPaddingFrames{0};
  std::atomic<uint64_t> audioLeadSilenceFrames{0};
  std::atomic<bool> audioPrimed{false};
  std::atomic<float> volume{1.0f};
  std::atomic<float> peak{0.0f};
  std::atomic<int64_t> clipAlertUntilUs{0};
  AudioSampleRing streamRb;
  std::atomic<bool> streamQueueEnabled{false};
  std::atomic<int> streamSerial{0};
  std::atomic<bool> streamBaseValid{false};
  std::atomic<int64_t> streamBasePtsUs{0};
  std::atomic<uint64_t> streamReadFrames{0};
  std::atomic<bool> streamClockReady{false};

  std::deque<AudioMetadata> streamMetadata;
  std::mutex streamMetadataMutex;
  std::condition_variable streamUpdateCv;
  std::atomic<uint64_t> streamUpdateCounter{0};

  std::atomic<bool> streamStarved{false};
  std::atomic<int64_t> streamDiscardPtsUs{0};
  uint64_t deviceDelayFrames = 0;
  Clock audioClock;
  uint32_t channels = 1;
  uint32_t sampleRate = 48000;
  bool dry = false;
  PcmToIfPreviewModulator radioPreview;
};

struct AuditionState {
  std::atomic<bool> active{false};
  std::atomic<bool> stop{false};
  std::thread worker;
  KssInstrumentDevice device = KssInstrumentDevice::None;
  uint32_t hash = 0;
  std::filesystem::path resumeFile;
  int resumeTrackIndex = 0;
  uint64_t resumeFrame = 0;
  bool resumePaused = false;
  bool resumeValid = false;
};

struct AudioPlaybackState {
  AudioState state{};
  Radio1938 radio1938Template{};
  bool deviceReady = false;
  bool decoderReady = false;
  bool enableAudio = false;
  bool dry = false;
  bool enableRadio = false;
  uint32_t sampleRate = 48000;
  uint32_t baseChannels = 1;
  uint32_t channels = 1;
  float lpHz = 0.0f;
  float noise = 0.0f;
  std::string radioSettingsPath;
  std::string radioPresetName;
  std::filesystem::path nowPlaying;
  int trackIndex = 0;
  std::string lastInitError;
  std::string gmeWarning;
  std::string gsfWarning;
  std::string vgmWarning;
  KssPlaybackOptions kssOptions{};
  NsfPlaybackOptions nsfOptions{};
  VgmPlaybackOptions vgmOptions{};
  std::filesystem::path vgmDevicesFile;
  std::vector<VgmDeviceInfo> vgmDevices;
  std::unordered_map<uint32_t, VgmDeviceOptions> vgmDeviceDefaults;
  std::unordered_map<uint32_t, VgmDeviceOptions> vgmDeviceOverrides;
  AuditionState audition{};
};

AudioPlaybackState gAudio;

static constexpr uint32_t kRadioProcessChannels = 1u;
static void audioStreamProcessRadioImpl(float* interleaved, uint32_t frames);
static bool backendUsesQueuedDecodeThread(const AudioBackendHandlers* backend);
static void clearQueuedPlaybackMetadata(AudioState* state);
static void resetQueuedPlaybackState(AudioState* state,
                                     uint64_t framePos,
                                     int serial);
static void stopDecodeWorker();
static void updateDeviceDelayFrames();
static bool writeQueuedSamples(AudioState* state,
                               const float* interleaved,
                               uint64_t frames,
                               int64_t ptsUs,
                               int serial,
                               bool allowBlock,
                               uint64_t* writtenFrames);
static bool startDecodeWorker(const AudioBackendHandlers* backend,
                              uint64_t startFrame);

static void rebuildRadioFromTemplate(Radio1938* target,
                                     const Radio1938& source,
                                     float sampleRate,
                                     float bwHz,
                                     float noise) {
  if (!target) return;
  *target = source;
  target->init(kRadioProcessChannels, sampleRate, bwHz, noise);
}

static void applyRadioSettingsToTemplate(Radio1938& target,
                                        const std::string& presetName,
                                        const std::string& settingsPath) {
  if (settingsPath.empty()) return;
  std::string error;
  if (!applyRadioSettingsIni(target, settingsPath, presetName, &error)) {
    std::fprintf(stderr,
                 "WARNING: Failed to apply radio settings from %s: %s\n",
                 settingsPath.c_str(),
                 error.c_str());
  }
}

void rebuildRadioPreviewChain(AudioState* state) {
  if (!state) return;
  rebuildRadioFromTemplate(&state->radio1938, gAudio.radio1938Template,
                           static_cast<float>(gAudio.sampleRate), gAudio.lpHz,
                           gAudio.noise);
  state->radioPreview.init(state->radio1938,
                           static_cast<float>(gAudio.sampleRate),
                           gAudio.lpHz);
}

void stopAuditionWorker() {
  if (!gAudio.audition.active.load()) return;
  gAudio.audition.stop.store(true);
  if (gAudio.audition.worker.joinable()) {
    gAudio.audition.worker.join();
  }
  gAudio.audition.stop.store(false);
  gAudio.audition.active.store(false);
  gAudio.audition.device = KssInstrumentDevice::None;
  gAudio.audition.hash = 0;
}

void applyRadioTogglePreset() {
  gAudio.radio1938Template.applyPreset(Radio1938::Preset::Philco37116X);
  applyRadioSettingsToTemplate(gAudio.radio1938Template,
                               gAudio.radioPresetName,
                               gAudio.radioSettingsPath);
  rebuildRadioPreviewChain(&gAudio.state);
}

void startAuditionWorker(AuditionTone tone) {
  gAudio.audition.stop.store(false);
  gAudio.audition.active.store(true);
  gAudio.audition.worker = std::thread([tone = std::move(tone)]() mutable {
    const uint32_t sampleRate = gAudio.sampleRate;
    const uint32_t channels = gAudio.channels;
    constexpr uint64_t kChunkFrames = 512;
    std::vector<float> buffer;
    buffer.resize(static_cast<size_t>(kChunkFrames) * channels);
    uint64_t framePos = 0;
    while (!gAudio.audition.stop.load()) {
      if (!gAudio.state.externalStream.load() ||
          !gAudio.state.streamQueueEnabled.load()) {
        break;
      }
      for (uint64_t i = 0; i < kChunkFrames; ++i) {
        float sample = renderAuditionSample(tone);
        for (uint32_t ch = 0; ch < channels; ++ch) {
          buffer[static_cast<size_t>(i * channels + ch)] = sample;
        }
      }
      audioStreamProcessRadioImpl(buffer.data(), static_cast<uint32_t>(kChunkFrames));
      uint64_t remaining = kChunkFrames;
      uint64_t offset = 0;
      while (remaining > 0 && !gAudio.audition.stop.load()) {
        int64_t ptsUs = static_cast<int64_t>(
            (framePos + offset) * 1000000ULL / sampleRate);
        uint64_t written = 0;
        if (!audioStreamWriteSamples(
                buffer.data() + static_cast<size_t>(offset) * channels,
                remaining, ptsUs, 0, false, &written)) {
          remaining = 0;
          break;
        }
        if (written == 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        remaining -= written;
        offset += written;
      }
      framePos += offset;
    }
    if (tone.kind == AuditionKind::Psg && tone.psg) {
      PSG_delete(tone.psg);
      tone.psg = nullptr;
    }
    if (tone.kind == AuditionKind::SccWave && tone.scc) {
      SCC_delete(tone.scc);
      tone.scc = nullptr;
    }
    gAudio.audition.active.store(false);
  });
}

static void updatePeakMeter(AudioState* state, const float* samples,
                            ma_uint32 frameCount) {
  if (!state || !samples || frameCount == 0) {
    if (state) {
      state->peak.store(0.0f, std::memory_order_relaxed);
    }
    return;
  }
  const uint32_t channels = std::max(1u, state->channels);
  const size_t count = static_cast<size_t>(frameCount) * channels;
  float peak = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    peak = std::max(peak, std::fabs(samples[i]));
  }
  float prev = state->peak.load(std::memory_order_relaxed);
  float rate = static_cast<float>(std::max(1u, state->sampleRate));
  float decay = std::exp(-static_cast<float>(frameCount) / (rate * 0.30f));
  float next = std::max(peak, prev * decay);
  state->peak.store(next, std::memory_order_relaxed);
}

static void holdClipAlert(AudioState* state) {
  if (!state) return;
  constexpr int64_t kClipAlertHoldUs = 900000;
  state->clipAlertUntilUs.store(nowUs() + kClipAlertHoldUs,
                                std::memory_order_relaxed);
}

static void processRadioBlock(AudioState* state,
                              float* samples,
                              uint32_t frames) {
  if (!state || !samples || frames == 0) return;
  const uint32_t channels = std::max(1u, state->channels);
  state->radioPreview.processBlock(state->radio1938, samples, frames, channels);
  if (state->radio1938.diagnostics.anyClip) {
    holdClipAlert(state);
  }
}

static void audioStreamProcessRadioImpl(float* interleaved, uint32_t frames) {
  if (!interleaved || frames == 0) return;
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) return;
  if (!gAudio.state.streamQueueEnabled.load()) return;
  if (gAudio.state.dry || !gAudio.state.useRadio1938.load()) return;
  processRadioBlock(&gAudio.state, interleaved, frames);
}

static bool backendUsesQueuedDecodeThread(const AudioBackendHandlers* backend) {
  return backend && backend->mode != AudioMode::None &&
         backend->mode != AudioMode::Stream &&
         backend->mode != AudioMode::M4a;
}

static void clearQueuedPlaybackMetadata(AudioState* state) {
  if (!state) return;
  std::lock_guard<std::mutex> lock(state->streamMetadataMutex);
  state->streamMetadata.clear();
}

static void resetQueuedPlaybackState(AudioState* state,
                                     uint64_t framePos,
                                     int serial) {
  if (!state) return;
  state->streamRb.reset();
  state->framesPlayed.store(framePos, std::memory_order_relaxed);
  state->audioClockFrames.store(framePos, std::memory_order_relaxed);
  state->finished.store(false, std::memory_order_relaxed);
  state->m4aAtEnd.store(false, std::memory_order_relaxed);
  state->audioPrimed.store(false, std::memory_order_relaxed);
  state->streamBaseValid.store(false, std::memory_order_relaxed);
  state->streamBasePtsUs.store(0, std::memory_order_relaxed);
  state->streamReadFrames.store(0, std::memory_order_relaxed);
  state->streamClockReady.store(false, std::memory_order_relaxed);
  state->streamStarved.store(false, std::memory_order_relaxed);
  state->streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  state->audioClock.reset(serial);
  clearQueuedPlaybackMetadata(state);
  state->decodeCv.notify_all();
}

static void stopDecodeWorker() {
  gAudio.state.decodeStop.store(true, std::memory_order_relaxed);
  gAudio.state.decodeCv.notify_all();
  if (gAudio.state.decodeThread.joinable()) {
    gAudio.state.decodeThread.join();
  }
  gAudio.state.decodeThreadRunning.store(false, std::memory_order_relaxed);
  gAudio.state.decodeStop.store(false, std::memory_order_relaxed);
}

static bool queuedDecodeReachedKnownEnd(const AudioState* state,
                                        uint64_t framePos) {
  if (!state) return false;
  uint64_t totalFrames = state->totalFrames.load(std::memory_order_relaxed);
  return totalFrames > 0 && framePos >= totalFrames;
}

static bool waitForQueuedDecodeRetry(AudioState* state) {
  if (!state) return false;
  std::unique_lock<std::mutex> lock(state->decodeMutex);
  state->decodeCv.wait_for(lock, std::chrono::milliseconds(5), [&]() {
    return state->decodeStop.load(std::memory_order_relaxed) ||
           state->seekRequested.load(std::memory_order_relaxed) ||
           !state->streamQueueEnabled.load(std::memory_order_relaxed);
  });
  return !state->decodeStop.load(std::memory_order_relaxed) &&
         !state->seekRequested.load(std::memory_order_relaxed) &&
         state->streamQueueEnabled.load(std::memory_order_relaxed);
}

static void updateDeviceDelayFrames() {
  uint64_t latencyFrames = 0;
  if (gAudio.deviceReady &&
      ma_device_get_state(&gAudio.state.device) !=
          ma_device_state_uninitialized) {
    uint32_t internalRate = gAudio.state.device.playback.internalSampleRate;
    uint64_t internalBuffer = 0;
    uint32_t periodFrames = gAudio.state.device.playback.internalPeriodSizeInFrames;
    uint32_t periods = gAudio.state.device.playback.internalPeriods;
    if (periodFrames > 0 && periods > 0) {
      internalBuffer = static_cast<uint64_t>(periodFrames) *
                       static_cast<uint64_t>(periods);
    }
#ifdef MA_SUPPORT_WASAPI
    if (gAudio.state.device.wasapi.actualBufferSizeInFramesPlayback > 0) {
      internalBuffer = std::max<uint64_t>(
          internalBuffer, gAudio.state.device.wasapi.actualBufferSizeInFramesPlayback);
    }
#endif
    if (internalBuffer > 0) {
      latencyFrames +=
          rescaleFrames(internalBuffer, internalRate, gAudio.state.sampleRate);
    }
    latencyFrames +=
        static_cast<uint64_t>(gAudio.state.device.playback.intermediaryBufferLen);
    uint64_t converterLatency =
        ma_data_converter_get_output_latency(&gAudio.state.device.playback.converter);
    if (converterLatency > 0) {
      latencyFrames +=
          rescaleFrames(converterLatency, internalRate, gAudio.state.sampleRate);
    }
  }
  gAudio.state.deviceDelayFrames = latencyFrames;
}

static bool writeQueuedSamples(AudioState* state,
                               const float* interleaved,
                               uint64_t frames,
                               int64_t ptsUs,
                               int serial,
                               bool allowBlock,
                               uint64_t* writtenFrames) {
  if (writtenFrames) {
    *writtenFrames = 0;
  }
  if (!state || !state->streamQueueEnabled.load(std::memory_order_relaxed) ||
      !interleaved || frames == 0) {
    return false;
  }
  if (serial != state->streamSerial.load(std::memory_order_relaxed)) {
    return true;
  }

  int64_t discardThreshold =
      state->streamDiscardPtsUs.load(std::memory_order_relaxed);
  if (discardThreshold > 0 && ptsUs < discardThreshold) {
    if (writtenFrames) *writtenFrames = frames;
    return true;
  } else if (discardThreshold > 0 && ptsUs >= discardThreshold) {
    state->streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  }

  if (!state->streamBaseValid.load(std::memory_order_relaxed)) {
    state->streamBasePtsUs.store(ptsUs, std::memory_order_relaxed);
    state->streamBaseValid.store(true, std::memory_order_relaxed);
    state->streamReadFrames.store(0, std::memory_order_relaxed);
    state->streamClockReady.store(false, std::memory_order_relaxed);
  }

  if (ptsUs != AV_NOPTS_VALUE) {
    std::lock_guard<std::mutex> lock(state->streamMetadataMutex);
    if (state->streamMetadata.empty() ||
        state->streamMetadata.back().ptsUs != ptsUs ||
        state->streamMetadata.back().serial != serial) {
      state->streamMetadata.push_back(
          {state->streamRb.wpos.load(std::memory_order_relaxed), ptsUs, serial});
    }
  }

  uint64_t remaining = frames;
  const float* cursor = interleaved;
  uint64_t totalWritten = 0;
  while (remaining > 0) {
    if (!state->externalStream.load(std::memory_order_relaxed) &&
        state->seekRequested.load(std::memory_order_relaxed)) {
      return false;
    }
    if (serial != state->streamSerial.load(std::memory_order_relaxed)) {
      return true;
    }
    if (!state->streamQueueEnabled.load(std::memory_order_relaxed)) {
      return false;
    }
    uint64_t written = state->streamRb.writeSome(cursor, remaining);
    if (written == 0) {
      if (!allowBlock) {
        break;
      }
      std::unique_lock<std::mutex> lock(state->decodeMutex);
      state->decodeCv.wait_for(lock, std::chrono::milliseconds(5), [&]() {
        return state->decodeStop.load(std::memory_order_relaxed) ||
               state->seekRequested.load(std::memory_order_relaxed) ||
               !state->streamQueueEnabled.load(std::memory_order_relaxed) ||
               serial != state->streamSerial.load(std::memory_order_relaxed) ||
               state->streamRb.writableFrames() > 0;
      });
      continue;
    }
    remaining -= written;
    totalWritten += written;
    cursor += static_cast<size_t>(written) * state->channels;
  }
  if (writtenFrames) {
    *writtenFrames = totalWritten;
  }
  return true;
}

static bool startDecodeWorker(const AudioBackendHandlers* backend,
                              uint64_t startFrame) {
  if (!backendUsesQueuedDecodeThread(backend) || !backend->read) {
    return false;
  }
  stopDecodeWorker();
  gAudio.state.decodeStop.store(false, std::memory_order_relaxed);
  gAudio.state.decodeThreadRunning.store(true, std::memory_order_relaxed);
  const uint32_t workerChannels = gAudio.channels;
  const uint32_t workerRate = gAudio.sampleRate;
  const bool finishOnShortRead = backend->finishOnShortRead;
  gAudio.state.decodeThread =
      std::thread([backend, startFrame, workerChannels, workerRate,
                   finishOnShortRead]() {
        constexpr uint32_t kChunkFrames = 2048;
        constexpr uint32_t kQueuedDecodeEmptyReadLimit = 32;
        std::vector<float> buffer(static_cast<size_t>(kChunkFrames) *
                                  workerChannels);
        uint64_t framePos = startFrame;
        uint32_t consecutiveEmptyReads = 0;
        while (!gAudio.state.decodeStop.load(std::memory_order_relaxed)) {
          if (backend->seek &&
              gAudio.state.seekRequested.exchange(false,
                                                 std::memory_order_relaxed)) {
            int64_t target = gAudio.state.pendingSeekFrames.load(
                std::memory_order_relaxed);
            if (target < 0) target = 0;
            uint64_t total = gAudio.state.totalFrames.load(std::memory_order_relaxed);
            if (total > 0 && static_cast<uint64_t>(target) > total) {
              target = static_cast<int64_t>(total);
            }
            uint64_t targetFrame = static_cast<uint64_t>(target);
            if (!backend->seek(targetFrame)) {
              targetFrame = 0;
              backend->seek(0);
            }
            framePos = targetFrame;
            consecutiveEmptyReads = 0;
            resetQueuedPlaybackState(&gAudio.state, targetFrame, 0);
            continue;
          }

          uint64_t writable = gAudio.state.streamRb.writableFrames();
          if (writable == 0) {
            std::unique_lock<std::mutex> lock(gAudio.state.decodeMutex);
            gAudio.state.decodeCv.wait_for(
                lock, std::chrono::milliseconds(5), [&]() {
                  return gAudio.state.decodeStop.load(std::memory_order_relaxed) ||
                         gAudio.state.seekRequested.load(std::memory_order_relaxed) ||
                         !gAudio.state.streamQueueEnabled.load(
                             std::memory_order_relaxed) ||
                         gAudio.state.streamRb.writableFrames() > 0;
                });
            continue;
          }

          uint32_t framesToRead =
              static_cast<uint32_t>(std::min<uint64_t>(writable, kChunkFrames));
          uint64_t framesRead = 0;
          if (!backend->read(buffer.data(), framesToRead, &framesRead)) {
            gAudio.state.m4aAtEnd.store(true, std::memory_order_relaxed);
            break;
          }

          bool reachedEnd = false;
          if (framesRead == 0) {
            if (queuedDecodeReachedKnownEnd(&gAudio.state, framePos)) {
              reachedEnd = true;
            } else if (++consecutiveEmptyReads >=
                       kQueuedDecodeEmptyReadLimit) {
              reachedEnd = true;
            } else {
              waitForQueuedDecodeRetry(&gAudio.state);
            }
          } else if (finishOnShortRead && framesRead < framesToRead &&
                     queuedDecodeReachedKnownEnd(&gAudio.state,
                                                framePos + framesRead)) {
            reachedEnd = true;
          }

          if (framesRead > 0) {
            consecutiveEmptyReads = 0;
            if (!gAudio.state.dry && gAudio.state.useRadio1938.load()) {
              processRadioBlock(&gAudio.state, buffer.data(),
                                static_cast<uint32_t>(framesRead));
            }
            uint64_t remaining = framesRead;
            uint64_t offset = 0;
            while (remaining > 0 &&
                   !gAudio.state.decodeStop.load(std::memory_order_relaxed)) {
              uint64_t written = 0;
              int64_t ptsUs = static_cast<int64_t>(
                  (framePos + offset) * 1000000ULL / workerRate);
              if (!writeQueuedSamples(
                      &gAudio.state,
                      buffer.data() +
                          static_cast<size_t>(offset) * workerChannels,
                      remaining, ptsUs, 0, true, &written)) {
                remaining = 0;
                break;
              }
              if (written == 0) {
                continue;
              }
              remaining -= written;
              offset += written;
            }
            framePos += offset;
            if (queuedDecodeReachedKnownEnd(&gAudio.state, framePos)) {
              reachedEnd = true;
            }
          }

          if (reachedEnd) {
            gAudio.state.m4aAtEnd.store(true, std::memory_order_relaxed);
            break;
          }
        }
        gAudio.state.decodeThreadRunning.store(false, std::memory_order_relaxed);
        gAudio.state.decodeCv.notify_all();
      });
  return true;
}

static inline float softLimitOutputSample(float x, float threshold = 0.985f) {
  float ax = std::fabs(x);
  if (ax <= threshold) return x;
  float sign = (x < 0.0f) ? -1.0f : 1.0f;
  float denom = std::max(1e-6f, 1.0f - threshold);
  float u = (ax - threshold) / denom;
  float y = threshold + (1.0f - std::exp(-u)) * (1.0f - threshold);
  return sign * y;
}

static bool applyOutputVolumeSafety(float* samples,
                                    uint32_t frames,
                                    uint32_t channels,
                                    float volume) {
  if (!samples || frames == 0 || channels == 0) return false;
  constexpr float kOutputClamp = 0.999f;
  bool limited = false;
  size_t count = static_cast<size_t>(frames) * channels;
  for (size_t i = 0; i < count; ++i) {
    float x = samples[i] * volume;
    if (std::fabs(x) > 1.0f) limited = true;
    x = softLimitOutputSample(x);
    if (std::fabs(x) >= kOutputClamp) limited = true;
    samples[i] = std::clamp(x, -kOutputClamp, kOutputClamp);
  }
  return limited;
}

void dataCallback(ma_device* device, void* output, const void*,
                  ma_uint32 frameCount) {
  auto* state = static_cast<AudioState*>(device->pUserData);
  float* out = static_cast<float*>(output);
  if (!state) return;

  state->callbackCount.fetch_add(1, std::memory_order_relaxed);
  state->framesRequested.fetch_add(frameCount, std::memory_order_relaxed);
  state->lastCallbackFrames.store(frameCount, std::memory_order_relaxed);
  state->audioPrimed.store(true, std::memory_order_relaxed);

  bool useStreamQueue = state->streamQueueEnabled.load();
  if (useStreamQueue) {
    const uint32_t channels = state->channels;
    const bool paused = state->paused.load() || state->hold.load();
    if (paused) {
      state->pausedCallbacks.fetch_add(1, std::memory_order_relaxed);
      state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
      state->lastFramesRead.store(0, std::memory_order_relaxed);
      std::fill(out, out + frameCount * channels, 0.0f);
      updatePeakMeter(state, out, frameCount);
      state->audioClock.set_paused(true, nowUs());
      state->streamStarved.store(false, std::memory_order_relaxed);
      return;
    }

    uint64_t framesRead =
        state->streamRb.readSome(out, static_cast<uint64_t>(frameCount));
    if (framesRead < frameCount) {
      std::fill(out + framesRead * channels,
                out + frameCount * channels, 0.0f);
    }
    if (framesRead > 0) {
      state->decodeCv.notify_one();
    }

    state->framesPlayed.fetch_add(framesRead, std::memory_order_relaxed);
    state->framesReadTotal.fetch_add(framesRead, std::memory_order_relaxed);
    state->lastFramesRead.store(static_cast<uint32_t>(framesRead),
                                std::memory_order_relaxed);
    state->audioClockFrames.fetch_add(framesRead, std::memory_order_relaxed);
    if (framesRead < frameCount) {
      state->shortReadCount.fetch_add(1, std::memory_order_relaxed);
      state->silentFrames.fetch_add(frameCount - framesRead,
                                    std::memory_order_relaxed);
    }

    if (framesRead > 0) {
      uint64_t currentRpos = state->streamRb.rpos.load(std::memory_order_relaxed);
      int64_t currentPts = AV_NOPTS_VALUE;
      int currentSerial = state->streamSerial.load(std::memory_order_relaxed);

      {
        std::lock_guard<std::mutex> lock(state->streamMetadataMutex);
        // Task 1: Find the most recent metadata entry that is AT or BEFORE our current read position.
        // This ensures the clock is always tied to the exact chunk of audio being played.
        while (state->streamMetadata.size() > 1 && 
               state->streamMetadata[1].wpos <= currentRpos) {
          state->streamMetadata.pop_front();
        }
        if (!state->streamMetadata.empty()) {
          const auto& md = state->streamMetadata.front();
          if (currentRpos >= md.wpos) {
              uint64_t offset = currentRpos - md.wpos;
              currentPts = md.ptsUs + static_cast<int64_t>(offset * 1000000ULL / state->sampleRate);
              currentSerial = md.serial;
          }
        }
      }

      if (currentPts != AV_NOPTS_VALUE) {
        // Compensate for hardware latency: the clock represents what is currently being heard.
        uint64_t delay = state->deviceDelayFrames;
        int64_t delayUs = static_cast<int64_t>(delay * 1000000ULL / state->sampleRate);
        
        state->audioClock.sync_to_pts(currentPts - delayUs, nowUs(), currentSerial);
        state->streamClockReady.store(true, std::memory_order_relaxed);
        
        // Task 3: Signal that audio hardware has progressed (Hardware Callback Driving)
        state->streamUpdateCounter.fetch_add(1, std::memory_order_release);
        state->streamUpdateCv.notify_all();
      }
    }

    bool starved = (framesRead < frameCount);
    state->streamStarved.store(starved, std::memory_order_relaxed);
    
    // If we starved, check how long it's been since the last valid update.
    // We allow a small "free-run" window (150ms) to bridge decoder hiccups 
    // without freezing the video clock. This prevents micro-stutters.
    if (starved) {
      int64_t now = nowUs();
      int64_t last = state->audioClock.last_updated_us.load(std::memory_order_relaxed);
      if (now - last > 150000) { // 150ms threshold
        state->audioClock.invalidate();
        state->streamClockReady.store(false, std::memory_order_relaxed);
      }
    }

    if (starved && state->m4aAtEnd.load()) {
      state->finished.store(true);
      uint64_t total = state->totalFrames.load();
      if (total > 0) {
        state->framesPlayed.store(total, std::memory_order_relaxed);
      }
    }

    float vol = state->volume.load(std::memory_order_relaxed);
    if (applyOutputVolumeSafety(out, frameCount, channels, vol)) {
      holdClipAlert(state);
    }
    updatePeakMeter(state, out, frameCount);
    return;
  }

  if (state->hold.load()) {
    state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    std::fill(out, out + frameCount * state->channels, 0.0f);
    updatePeakMeter(state, out, frameCount);
    return;
  }

  const AudioMode mode = state->mode.load(std::memory_order_relaxed);
  const AudioBackendHandlers* backend = state->backend;
  if (backend && backend->allowSeekInCallback && backend->seek &&
      state->seekRequested.exchange(false)) {
    int64_t target = state->pendingSeekFrames.load();
    if (target < 0) target = 0;
    if (!backend->seek(static_cast<uint64_t>(target))) {
      target = 0;
      backend->seek(0);
    }
    state->framesPlayed.store(static_cast<uint64_t>(target));
    state->framesRequested.store(static_cast<uint64_t>(target)); // Reset request count too
    state->audioClockFrames.store(static_cast<uint64_t>(target));
    state->finished.store(false);
  }

  if (state->paused.load()) {
    state->pausedCallbacks.fetch_add(1, std::memory_order_relaxed);
    state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    std::fill(out, out + frameCount * state->channels, 0.0f);
    return;
  }

  uint32_t framesRemaining = frameCount;
  uint64_t silentLeadFrames = 0;
  uint64_t leadSilence = state->audioLeadSilenceFrames.load();
  float* outCursor = out;
  if (leadSilence > 0) {
    silentLeadFrames = std::min<uint64_t>(leadSilence, framesRemaining);
    if (silentLeadFrames > 0) {
      std::fill(outCursor, outCursor + silentLeadFrames * state->channels,
                0.0f);
      state->audioLeadSilenceFrames.fetch_sub(silentLeadFrames,
                                              std::memory_order_relaxed);
      framesRemaining -= static_cast<uint32_t>(silentLeadFrames);
      outCursor += silentLeadFrames * state->channels;
    }
  }

  uint64_t framesRead = 0;
  auto finishPlayback = [&]() {
    state->finished.store(true, std::memory_order_relaxed);
    uint64_t total = state->totalFrames.load(std::memory_order_relaxed);
    if (total > 0) {
      state->framesPlayed.store(total, std::memory_order_relaxed);
    } else {
      state->framesPlayed.store(0, std::memory_order_relaxed);
    }
    std::fill(outCursor, outCursor + framesRemaining * state->channels, 0.0f);
    framesRead = 0;
  };
  if (framesRemaining > 0) {
    if (!backend || !backend->read) {
      finishPlayback();
    } else {
      uint64_t backendFramesRead = 0;
      if (!backend->read(outCursor, framesRemaining, &backendFramesRead)) {
        finishPlayback();
      } else {
        framesRead = backendFramesRead;
        if (framesRead < framesRemaining) {
          std::fill(outCursor + framesRead * state->channels,
                    outCursor + framesRemaining * state->channels, 0.0f);
        }
        if (mode == AudioMode::M4a) {
          if (framesRead == 0 && state->m4aAtEnd.load()) {
            state->finished.store(true, std::memory_order_relaxed);
            uint64_t total = state->totalFrames.load(std::memory_order_relaxed);
            if (total > 0) {
              state->framesPlayed.store(total, std::memory_order_relaxed);
            }
          }
        } else if (backend->finishOnShortRead &&
                   (framesRead == 0 || framesRead < framesRemaining)) {
          finishPlayback();
        }
      }
    }
  }

  state->audioClockFrames.fetch_add(frameCount, std::memory_order_relaxed);
  if (!state->dry && framesRead > 0 && state->useRadio1938.load()) {
    float* audioStart =
        out + silentLeadFrames * static_cast<uint64_t>(state->channels);
    processRadioBlock(state, audioStart, static_cast<uint32_t>(framesRead));
  }
  state->framesPlayed.fetch_add(framesRead);
  state->framesReadTotal.fetch_add(framesRead, std::memory_order_relaxed);
  state->lastFramesRead.store(static_cast<uint32_t>(framesRead),
                              std::memory_order_relaxed);
  uint64_t silentFrames = silentLeadFrames;
  if (framesRead < framesRemaining) {
    silentFrames += static_cast<uint64_t>(framesRemaining - framesRead);
    state->shortReadCount.fetch_add(1, std::memory_order_relaxed);
  }
  if (silentFrames > 0) {
    state->silentFrames.fetch_add(silentFrames, std::memory_order_relaxed);
  }

  float vol = state->volume.load(std::memory_order_relaxed);
  if (applyOutputVolumeSafety(out, frameCount, state->channels, vol)) {
    holdClipAlert(state);
  }
  updatePeakMeter(state, out, frameCount);
}

void stopM4aWorker() {
  if (!gAudio.state.m4aThreadRunning.load()) return;
  gAudio.state.m4aStop.store(true);
  gAudio.state.m4aCv.notify_all();
  if (gAudio.state.m4aThread.joinable()) {
    gAudio.state.m4aThread.join();
  }
  gAudio.state.m4aThreadRunning.store(false);
  gAudio.state.m4aStop.store(false);
  gAudio.state.m4aAtEnd.store(false);
  {
    std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
    audioRingBufferReset(&gAudio.state.m4aBuffer);
    gAudio.state.m4aInitDone = false;
    gAudio.state.m4aInitOk = false;
    gAudio.state.m4aInitError.clear();
  }
}

bool startM4aWorker(const std::filesystem::path& file, uint64_t startFrame,
                    std::string* error) {
  stopM4aWorker();
  const uint32_t rbFrames = std::max<uint32_t>(gAudio.sampleRate / 2, 8192);
  const uint32_t targetFrames =
      std::min<uint32_t>(rbFrames, std::max<uint32_t>(gAudio.sampleRate / 4, 4096));
  {
    std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
    audioRingBufferInit(&gAudio.state.m4aBuffer, rbFrames, gAudio.channels);
    gAudio.state.m4aInitDone = false;
    gAudio.state.m4aInitOk = false;
    gAudio.state.m4aInitError.clear();
  }
  gAudio.state.m4aStop.store(false);
  gAudio.state.m4aAtEnd.store(false);
  gAudio.state.m4aThreadRunning.store(true);
  const uint32_t workerChannels = gAudio.channels;
  const uint32_t workerRate = gAudio.sampleRate;
  gAudio.state.m4aThread = std::thread([file, startFrame, workerChannels,
                                        workerRate, targetFrames]() {
    FfmpegAudioDecoder decoder;
    std::string initError;
    auto t_init_start = std::chrono::steady_clock::now();
    bool ok = decoder.init(file, workerChannels, workerRate, &initError);
    auto t_init_end = std::chrono::steady_clock::now();
#if RADIOIFY_ENABLE_TIMING_LOG
    {
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    "ffmpeg_audio_init_ms=%lld file=%s ok=%d err=%s",
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            t_init_end - t_init_start)
                            .count()),
                    toUtf8String(file.filename()).c_str(), ok ? 1 : 0,
                    initError.empty() ? "-" : initError.c_str());
      {
        std::ofstream f((std::filesystem::current_path() /
                         "radioify.log")
                            .string(),
                        std::ios::app);
        if (f) f << radioifyLogTimestamp() << " " << buf << "\n";
      }
    }
#endif
    uint64_t total = 0;
    uint64_t paddingFrames = 0;
    uint64_t trailingFrames = 0;
    int64_t startOffsetFrames = 0;
    uint64_t leadSilenceFrames = 0;
    uint64_t localStart = startFrame;
    uint64_t rawTotalFrames = 0;
    if (ok) {
      decoder.getPaddingFrames(&paddingFrames, &trailingFrames);
      decoder.getStartOffsetFrames(&startOffsetFrames);
      decoder.getTotalFrames(&total);
      if (startOffsetFrames > 0) {
        leadSilenceFrames = static_cast<uint64_t>(startOffsetFrames);
        startOffsetFrames = 0;
      } else if (startOffsetFrames < 0) {
        uint64_t extraSkip = static_cast<uint64_t>(-startOffsetFrames);
        paddingFrames += extraSkip;
        if (total > extraSkip) {
          total -= extraSkip;
        } else {
          total = 0;
        }
        startOffsetFrames = 0;
      }
      if (total > 0 && localStart > total) {
        localStart = total;
      }
      if (total > 0) {
        rawTotalFrames = total + paddingFrames + trailingFrames;
      }
      uint64_t rawStart = localStart + paddingFrames;
      if (rawTotalFrames > 0 && rawStart > rawTotalFrames) {
        rawStart = rawTotalFrames;
      }
      if (rawStart > 0) {
        if (!decoder.seekToFrame(rawStart)) {
          rawStart = 0;
          localStart = 0;
        }
      }
    }
    {
      std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
      gAudio.state.m4aInitDone = true;
      gAudio.state.m4aInitOk = ok;
      gAudio.state.m4aInitError = initError;
    }
    gAudio.state.totalFrames.store(total);
    gAudio.state.audioPaddingFrames.store(paddingFrames);
    gAudio.state.audioTrailingPaddingFrames.store(trailingFrames);
    gAudio.state.audioLeadSilenceFrames.store(leadSilenceFrames);
    if (localStart > 0) {
      gAudio.state.framesPlayed.store(localStart);
      gAudio.state.audioClockFrames.store(localStart);
    }
    gAudio.state.m4aCv.notify_all();
    if (!ok) {
      return;
    }

    constexpr uint32_t kChunkFrames = 2048;
    std::vector<float> buffer(static_cast<size_t>(kChunkFrames) *
                              workerChannels);
    bool m4a_first_buffer_logged = false;

    while (!gAudio.state.m4aStop.load()) {
      if (gAudio.state.seekRequested.load()) {
        int64_t target = gAudio.state.pendingSeekFrames.load();
        if (target < 0) target = 0;
        gAudio.state.seekRequested.store(false);
        uint64_t targetFrames = static_cast<uint64_t>(target);
        uint64_t rawTarget = targetFrames + paddingFrames;
        if (rawTotalFrames > 0 && rawTarget > rawTotalFrames) {
          rawTarget = rawTotalFrames;
        }
        if (!decoder.seekToFrame(rawTarget)) {
          rawTarget = 0;
          targetFrames = 0;
          decoder.seekToFrame(0);
        }
        gAudio.state.framesPlayed.store(targetFrames);
        gAudio.state.audioClockFrames.store(targetFrames);
        gAudio.state.finished.store(false);
        gAudio.state.m4aAtEnd.store(false);
        gAudio.state.audioLeadSilenceFrames.store(0);
        {
          std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
          audioRingBufferReset(&gAudio.state.m4aBuffer);
        }
      }

      if (gAudio.state.m4aAtEnd.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        continue;
      }

      size_t spaceFrames = 0;
      size_t bufferedFrames = 0;
      {
        std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
        spaceFrames = audioRingBufferSpace(&gAudio.state.m4aBuffer);
        bufferedFrames = audioRingBufferSize(&gAudio.state.m4aBuffer);
      }
      if (spaceFrames == 0 || bufferedFrames >= targetFrames) {
        std::unique_lock<std::mutex> lock(gAudio.state.m4aMutex);
        gAudio.state.m4aCv.wait_for(lock, std::chrono::milliseconds(5), [&]() {
          return (audioRingBufferSpace(&gAudio.state.m4aBuffer) > 0 &&
                  audioRingBufferSize(&gAudio.state.m4aBuffer) < targetFrames) ||
                 gAudio.state.m4aStop.load() ||
                 gAudio.state.seekRequested.load();
        });
        continue;
      }

      size_t desiredFrames =
          targetFrames > bufferedFrames ? (targetFrames - bufferedFrames) : 0;
      size_t toRead = std::min<size_t>(spaceFrames, kChunkFrames);
      if (desiredFrames > 0) {
        toRead = std::min(toRead, desiredFrames);
      }
      uint32_t framesToRead = static_cast<uint32_t>(toRead);
      uint64_t framesRead = 0;
      if (!decoder.readFrames(buffer.data(), framesToRead, &framesRead)) {
        gAudio.state.m4aAtEnd.store(true);
        continue;
      }
      if (framesRead == 0) {
        gAudio.state.m4aAtEnd.store(true);
        continue;
      }
      if (!m4a_first_buffer_logged) {
        m4a_first_buffer_logged = true;
#if RADIOIFY_ENABLE_TIMING_LOG
        auto t_buf = std::chrono::steady_clock::now();
        {
          char buf[512];
          std::snprintf(
              buf, sizeof(buf),
              "ffmpeg_audio_first_buffer_ms=%lld frames=%llu file=%s",
              static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      t_buf - t_init_start)
                      .count()),
              static_cast<unsigned long long>(framesRead),
              toUtf8String(file.filename()).c_str());
          std::ofstream f((std::filesystem::current_path() /
                           "radioify.log")
                              .string(),
                          std::ios::app);
          if (f) f << radioifyLogTimestamp() << " " << buf << "\n";
        }
#endif
      }
      {
        std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
        audioRingBufferWrite(&gAudio.state.m4aBuffer, buffer.data(),
                             static_cast<size_t>(framesRead), workerChannels);
      }
    }
  });

  {
    std::unique_lock<std::mutex> lock(gAudio.state.m4aMutex);
    gAudio.state.m4aCv.wait(lock, [&]() { return gAudio.state.m4aInitDone; });
    if (!gAudio.state.m4aInitOk && error) {
      *error = gAudio.state.m4aInitError;
    }
  }
  if (!gAudio.state.m4aInitOk) {
    stopM4aWorker();
    return false;
  }
  return true;
}

bool initDevice() {
  if (gAudio.deviceReady) return true;
  ma_device_config devConfig = ma_device_config_init(ma_device_type_playback);
  devConfig.playback.format = ma_format_f32;
  devConfig.playback.channels = gAudio.channels;
  devConfig.sampleRate = gAudio.sampleRate;
  devConfig.dataCallback = dataCallback;
  devConfig.pUserData = &gAudio.state;

  if (ma_device_init(nullptr, &devConfig, &gAudio.state.device) != MA_SUCCESS) {
    return false;
  }
  if (ma_device_start(&gAudio.state.device) != MA_SUCCESS) {
    ma_device_uninit(&gAudio.state.device);
    return false;
  }
  gAudio.deviceReady = true;
#if RADIOIFY_ENABLE_TIMING_LOG
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "audio_device_started sampleRate=%u channels=%u",
                  gAudio.sampleRate, gAudio.channels);
    std::ofstream f((std::filesystem::current_path() / "radioify.log")
                        .string(),
                    std::ios::app);
    if (f) f << radioifyLogTimestamp() << " " << buf << "\n";
  }
#endif
  return true;
}

void applyVgmDeviceOverrides() {
  if (gAudio.state.mode.load(std::memory_order_relaxed) != AudioMode::Vgm) {
    return;
  }
  if (gAudio.vgmDeviceOverrides.empty()) return;
  for (const auto& entry : gAudio.vgmDeviceOverrides) {
    gAudio.state.vgm.setDeviceOptions(entry.first, entry.second);
  }
}

const VgmDeviceInfo* findVgmDeviceInfo(uint32_t deviceId) {
  for (const auto& device : gAudio.vgmDevices) {
    if (device.id == deviceId) return &device;
  }
  return nullptr;
}

AudioMode currentAudioMode() {
  return gAudio.state.mode.load(std::memory_order_relaxed);
}

bool isAudioMode(AudioMode mode) {
  return currentAudioMode() == mode;
}

bool initM4aBackend(const std::filesystem::path& file, uint64_t startFrame,
                    int, std::string* error) {
  gAudio.state.totalFrames.store(0);
  return startM4aWorker(file, startFrame, error);
}

bool initFfmpegBackend(const std::filesystem::path& file, uint64_t, int,
                       std::string* error) {
  gAudio.state.totalFrames.store(0);
  return gAudio.state.ffmpeg.init(file, gAudio.channels, gAudio.sampleRate, error);
}

void uninitM4aBackend() { stopM4aWorker(); }

void uninitFfmpegBackend() { gAudio.state.ffmpeg.uninit(); }

bool readFfmpegBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.ffmpeg.readFrames(out, frameCount, framesRead);
}

bool seekFfmpegBackend(uint64_t frame) {
  return gAudio.state.ffmpeg.seekToFrame(frame);
}

bool totalFfmpegBackend(uint64_t* outFrames) {
  if (!outFrames) return false;
  return gAudio.state.ffmpeg.getTotalFrames(outFrames);
}

bool readM4aBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!out || frameCount == 0) return false;

  if (!gAudio.state.externalStream.load() && gAudio.state.seekRequested.load()) {
    std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
    audioRingBufferReset(&gAudio.state.m4aBuffer);
    gAudio.state.m4aAtEnd.store(false);
    gAudio.state.finished.store(false);
  }

  size_t readFrames = 0;
  {
    std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
    readFrames = audioRingBufferRead(&gAudio.state.m4aBuffer, out, frameCount,
                                     gAudio.state.channels);
  }
  if (framesRead) *framesRead = static_cast<uint64_t>(readFrames);
  gAudio.state.m4aCv.notify_one();
  return true;
}

bool totalM4aBackend(uint64_t* outFrames) {
  if (!outFrames) return false;
  *outFrames = gAudio.state.totalFrames.load(std::memory_order_relaxed);
  return true;
}

bool initFlacBackend(const std::filesystem::path& file, uint64_t, int,
                     std::string* error) {
  return gAudio.state.flac.init(file, gAudio.channels, gAudio.sampleRate, error);
}

void uninitFlacBackend() { gAudio.state.flac.uninit(); }

bool readFlacBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.flac.readFrames(out, frameCount, framesRead);
}

bool seekFlacBackend(uint64_t frame) { return gAudio.state.flac.seekToFrame(frame); }

bool totalFlacBackend(uint64_t* outFrames) {
  return gAudio.state.flac.getTotalFrames(outFrames);
}

bool initKssBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return gAudio.state.kss.init(file, gAudio.channels, gAudio.sampleRate, error,
                               trackIndex, gAudio.kssOptions);
}

void uninitKssBackend() { gAudio.state.kss.uninit(); }

bool readKssBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.kss.readFrames(out, frameCount, framesRead);
}

bool seekKssBackend(uint64_t frame) { return gAudio.state.kss.seekToFrame(frame); }

bool totalKssBackend(uint64_t* outFrames) {
  return gAudio.state.kss.getTotalFrames(outFrames);
}

bool initPsfBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return gAudio.state.psf.init(file, gAudio.channels, gAudio.sampleRate, error,
                               trackIndex);
}

void uninitPsfBackend() { gAudio.state.psf.uninit(); }

bool readPsfBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.psf.readFrames(out, frameCount, framesRead);
}

bool seekPsfBackend(uint64_t frame) { return gAudio.state.psf.seekToFrame(frame); }

bool totalPsfBackend(uint64_t* outFrames) {
  return gAudio.state.psf.getTotalFrames(outFrames);
}

bool initGsfBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return gAudio.state.gsf.init(file, gAudio.channels, gAudio.sampleRate, error,
                               trackIndex);
}

void uninitGsfBackend() { gAudio.state.gsf.uninit(); }

bool readGsfBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.gsf.readFrames(out, frameCount, framesRead);
}

bool seekGsfBackend(uint64_t frame) { return gAudio.state.gsf.seekToFrame(frame); }

bool totalGsfBackend(uint64_t* outFrames) {
  return gAudio.state.gsf.getTotalFrames(outFrames);
}

bool initVgmBackend(const std::filesystem::path& file, uint64_t, int,
                    std::string* error) {
  if (!gAudio.state.vgm.init(file, gAudio.channels, gAudio.sampleRate, error)) {
    return false;
  }
  gAudio.state.vgm.applyOptions(gAudio.vgmOptions);
  applyVgmDeviceOverrides();
  gAudio.vgmWarning = gAudio.state.vgm.warning();
  return true;
}

void uninitVgmBackend() { gAudio.state.vgm.uninit(); }

bool readVgmBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.vgm.readFrames(out, frameCount, framesRead);
}

bool seekVgmBackend(uint64_t frame) { return gAudio.state.vgm.seekToFrame(frame); }

bool totalVgmBackend(uint64_t* outFrames) {
  return gAudio.state.vgm.getTotalFrames(outFrames);
}

bool initGmeBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  if (!gAudio.state.gme.init(file, gAudio.channels, gAudio.sampleRate, error,
                             trackIndex)) {
    return false;
  }
  gAudio.state.gme.applyNsfOptions(gAudio.nsfOptions);
  gAudio.gmeWarning = gAudio.state.gme.warning();
  return true;
}

void uninitGmeBackend() { gAudio.state.gme.uninit(); }

bool readGmeBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.gme.readFrames(out, frameCount, framesRead);
}

bool seekGmeBackend(uint64_t frame) { return gAudio.state.gme.seekToFrame(frame); }

bool totalGmeBackend(uint64_t* outFrames) {
  return gAudio.state.gme.getTotalFrames(outFrames);
}

bool initMidiBackend(const std::filesystem::path& file, uint64_t, int,
                     std::string* error) {
  return gAudio.state.midi.init(file, gAudio.channels, gAudio.sampleRate, error);
}

void uninitMidiBackend() { gAudio.state.midi.uninit(); }

bool readMidiBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.midi.readFrames(out, frameCount, framesRead);
}

bool seekMidiBackend(uint64_t frame) { return gAudio.state.midi.seekToFrame(frame); }

bool totalMidiBackend(uint64_t* outFrames) {
  return gAudio.state.midi.getTotalFrames(outFrames);
}

bool initMiniaudioBackend(const std::filesystem::path& file, uint64_t, int,
                          std::string*) {
  ma_decoder_config decConfig =
      ma_decoder_config_init(ma_format_f32, gAudio.channels, gAudio.sampleRate);
  return ma_decoder_init_file(file.string().c_str(), &decConfig,
                              &gAudio.state.decoder) == MA_SUCCESS;
}

void uninitMiniaudioBackend() { ma_decoder_uninit(&gAudio.state.decoder); }

bool readMiniaudioBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  ma_uint64 read = 0;
  ma_result result =
      ma_decoder_read_pcm_frames(&gAudio.state.decoder, out, frameCount, &read);
  if (framesRead) *framesRead = static_cast<uint64_t>(read);
  return result == MA_SUCCESS || result == MA_AT_END;
}

bool seekMiniaudioBackend(uint64_t frame) {
  return ma_decoder_seek_to_pcm_frame(&gAudio.state.decoder,
                                      static_cast<ma_uint64>(frame)) ==
         MA_SUCCESS;
}

bool totalMiniaudioBackend(uint64_t* outFrames) {
  if (!outFrames) return false;
  ma_uint64 total = 0;
  if (ma_decoder_get_length_in_pcm_frames(&gAudio.state.decoder, &total) !=
      MA_SUCCESS) {
    return false;
  }
  *outFrames = static_cast<uint64_t>(total);
  return true;
}

std::string warningGmeBackend() { return gAudio.gmeWarning; }
std::string warningGsfBackend() { return gAudio.gsfWarning; }
std::string warningVgmBackend() { return gAudio.vgmWarning; }

const AudioBackendHandlers kBackendM4a{
    AudioMode::M4a, false, false, false, true, initM4aBackend, uninitM4aBackend,
    readM4aBackend, nullptr, totalM4aBackend, nullptr};
const AudioBackendHandlers kBackendFfmpeg{
    AudioMode::Ffmpeg, false, true, true, true, initFfmpegBackend,
    uninitFfmpegBackend, readFfmpegBackend, seekFfmpegBackend,
    totalFfmpegBackend, nullptr};
const AudioBackendHandlers kBackendFlac{
    AudioMode::Flac, false, true, true, true, initFlacBackend, uninitFlacBackend,
    readFlacBackend, seekFlacBackend, totalFlacBackend, nullptr};
const AudioBackendHandlers kBackendKss{
    AudioMode::Kss, true, true, true, true, initKssBackend, uninitKssBackend,
    readKssBackend, seekKssBackend, totalKssBackend, nullptr};
const AudioBackendHandlers kBackendPsf{
    AudioMode::Psf, true, true, true, false, initPsfBackend, uninitPsfBackend,
    readPsfBackend, seekPsfBackend, totalPsfBackend, nullptr};
const AudioBackendHandlers kBackendGsf{
    AudioMode::Gsf, true, true, true, false, initGsfBackend, uninitGsfBackend,
    readGsfBackend, seekGsfBackend, totalGsfBackend, warningGsfBackend};
const AudioBackendHandlers kBackendVgm{
    AudioMode::Vgm, true, true, true, true, initVgmBackend, uninitVgmBackend,
    readVgmBackend, seekVgmBackend, totalVgmBackend, warningVgmBackend};
const AudioBackendHandlers kBackendGme{
    AudioMode::Gme, true, true, true, true, initGmeBackend, uninitGmeBackend,
    readGmeBackend, seekGmeBackend, totalGmeBackend, warningGmeBackend};
const AudioBackendHandlers kBackendMidi{
    AudioMode::Midi, false, true, true, true, initMidiBackend, uninitMidiBackend,
    readMidiBackend, seekMidiBackend, totalMidiBackend, nullptr};
const AudioBackendHandlers kBackendMiniaudio{
    AudioMode::Miniaudio, false, true, true, true, initMiniaudioBackend,
    uninitMiniaudioBackend, readMiniaudioBackend, seekMiniaudioBackend,
    totalMiniaudioBackend, nullptr};

struct BackendSelector {
  bool (*matches)(const std::filesystem::path& file);
  const AudioBackendHandlers* backend;
};

const BackendSelector kBackends[] = {
    {isM4aExt, &kBackendM4a},         {isOggExt, &kBackendFfmpeg},
    {isFlacExt, &kBackendFlac},
    {isMiniaudioExt, &kBackendMiniaudio}, {isGmeExt, &kBackendGme},
    {isMidiExt, &kBackendMidi},       {isGsfExt, &kBackendGsf},
    {isVgmExt, &kBackendVgm},         {isKssExt, &kBackendKss},
    {isPsfExt, &kBackendPsf},
};

const AudioBackendHandlers* selectAudioBackend(const std::filesystem::path& file) {
  for (const auto& entry : kBackends) {
    if (entry.matches(file)) return entry.backend;
  }
  return nullptr;
}

std::string warningForBackend(const AudioBackendHandlers* backend) {
  if (backend && backend->warning) {
    return backend->warning();
  }
  return {};
}

void activateBackend(const AudioBackendHandlers* backend, int trackIndex) {
  gAudio.decoderReady = true;
  gAudio.state.backend = backend;
  gAudio.state.mode.store(backend ? backend->mode : AudioMode::None,
                          std::memory_order_relaxed);
  gAudio.state.externalStream.store(false);
  gAudio.trackIndex = (backend && backend->supportsTrackIndex) ? trackIndex : 0;
}

void storeTotalFramesFromBackend(const AudioBackendHandlers* backend) {
  if (!backend || !backend->totalFrames) {
    return;
  }
  uint64_t total = 0;
  bool ok = backend->totalFrames(&total);
  gAudio.state.totalFrames.store(ok ? total : 0);
}

void stopAndUninitActiveDecoder() {
  stopDecodeWorker();
  if (gAudio.decoderReady) {
    const AudioBackendHandlers* backend = gAudio.state.backend;
    if (backend && backend->uninit) {
      backend->uninit();
    }
  }
  gAudio.decoderReady = false;
  gAudio.state.backend = nullptr;
  gAudio.state.mode.store(AudioMode::None, std::memory_order_relaxed);
  gAudio.state.externalStream.store(false);
  gAudio.state.streamQueueEnabled.store(false, std::memory_order_relaxed);
  gAudio.state.streamSerial.store(0, std::memory_order_relaxed);
  gAudio.state.streamBaseValid.store(false, std::memory_order_relaxed);
  gAudio.state.streamBasePtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.m4aAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.streamRb.reset();
  gAudio.state.audioClock.reset(0);
  clearQueuedPlaybackMetadata(&gAudio.state);
  gAudio.state.decodeCv.notify_all();
}

bool initDecoderForBackend(const AudioBackendHandlers* backend,
                           const std::filesystem::path& file,
                           uint64_t startFrame, int trackIndex) {
  if (!backend || !backend->init) return false;
  std::string error;
  if (!backend->init(file, startFrame, trackIndex, &error)) {
    gAudio.lastInitError = error;
    if (backend->mode == AudioMode::Gsf) {
      gAudio.gsfWarning = error;
    } else if (backend->mode == AudioMode::Vgm) {
      gAudio.vgmWarning = error;
    } else {
      gAudio.gmeWarning = error;
    }
    return false;
  }
  gAudio.lastInitError.clear();
  activateBackend(backend, trackIndex);
  storeTotalFramesFromBackend(backend);
  return true;
}

void seekLoadedDecoderToStart(const AudioBackendHandlers* backend,
                              uint64_t* startFrame) {
  if (!backend || !startFrame || !backend->seek) return;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total > 0 && *startFrame > total) {
    *startFrame = total;
  }
  if (*startFrame == 0) return;
  if (!backend->seek(*startFrame)) {
    *startFrame = 0;
    backend->seek(0);
  }
}

void resetPlaybackStateForLoad(uint64_t startFrame) {
  gAudio.state.framesPlayed.store(startFrame);
  gAudio.state.audioClockFrames.store(startFrame);
  gAudio.state.callbackCount.store(0);
  gAudio.state.framesRequested.store(0);
  gAudio.state.framesReadTotal.store(0);
  gAudio.state.shortReadCount.store(0);
  gAudio.state.silentFrames.store(0);
  gAudio.state.pausedCallbacks.store(0);
  gAudio.state.lastCallbackFrames.store(0);
  gAudio.state.lastFramesRead.store(0);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingSeekFrames.store(0);
  gAudio.state.finished.store(false);
  gAudio.state.paused.store(false);
  gAudio.state.audioPaddingFrames.store(0);
  gAudio.state.audioTrailingPaddingFrames.store(0);
  gAudio.state.audioLeadSilenceFrames.store(0);
  gAudio.state.streamQueueEnabled.store(false, std::memory_order_relaxed);
  gAudio.state.streamSerial.store(0, std::memory_order_relaxed);
  gAudio.state.streamBaseValid.store(false, std::memory_order_relaxed);
  gAudio.state.streamBasePtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.m4aAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.deviceDelayFrames = 0;

  gAudio.state.channels = gAudio.channels;
  rebuildRadioPreviewChain(&gAudio.state);
}

bool ensurePlaybackDeviceRunning() {
  if (!gAudio.deviceReady) {
    if (!initDevice()) {
      stopAndUninitActiveDecoder();
      return false;
    }
    return true;
  }

  if (ma_device_start(&gAudio.state.device) != MA_SUCCESS) {
    stopAndUninitActiveDecoder();
    return false;
  }
  return true;
}

bool loadFileAt(const std::filesystem::path& file, uint64_t startFrame,
                int trackIndex) {
  validateInputFile(file);
  melodyOfflineStop();
  gAudio.state.audioLeadSilenceFrames.store(0);
  if (gAudio.audition.active.load()) {
    stopAuditionWorker();
    gAudio.audition.resumeValid = false;
  }

  gAudio.lastInitError.clear();
  gAudio.gmeWarning.clear();
  gAudio.gsfWarning.clear();
  gAudio.vgmWarning.clear();

  const AudioBackendHandlers* backend = selectAudioBackend(file);
  if (!backend) {
    gAudio.lastInitError = "Unsupported audio format.";
    return false;
  }

  if (gAudio.deviceReady) {
    ma_device_stop(&gAudio.state.device);
  }
  stopAndUninitActiveDecoder();

  if (!initDecoderForBackend(backend, file, startFrame, trackIndex)) {
    return false;
  }

  seekLoadedDecoderToStart(backend, &startFrame);
  resetPlaybackStateForLoad(startFrame);

  if (backendUsesQueuedDecodeThread(backend)) {
    const uint32_t rbFrames = std::max<uint32_t>(gAudio.sampleRate / 2, 8192);
    gAudio.state.streamRb.init(rbFrames, gAudio.channels);
    gAudio.state.streamQueueEnabled.store(true, std::memory_order_relaxed);
    resetQueuedPlaybackState(&gAudio.state, startFrame, 0);
    if (!startDecodeWorker(backend, startFrame)) {
      stopAndUninitActiveDecoder();
      return false;
    }
  }

  if (!ensurePlaybackDeviceRunning()) {
    return false;
  }

  updateDeviceDelayFrames();

  const uint64_t analysisLeadInFrames = gAudio.state.audioLeadSilenceFrames.load();
  if (backend->allowConcurrentOfflineAnalysis) {
    melodyOfflineStart(file, trackIndex, gAudio.sampleRate, gAudio.channels,
                      analysisLeadInFrames, gAudio.kssOptions,
                      gAudio.nsfOptions, gAudio.vgmOptions,
                      gAudio.vgmDeviceOverrides);
  }

  gAudio.nowPlaying = file;
  return true;
}

bool ensureChannels(uint32_t newChannels) {
  if (newChannels == gAudio.channels) return true;

  uint64_t resumeFrame = gAudio.decoderReady ? gAudio.state.framesPlayed.load()
                                             : 0;
  bool hadTrack = gAudio.decoderReady && !gAudio.nowPlaying.empty();
  int trackIndex = gAudio.trackIndex;

  if (gAudio.deviceReady) {
    ma_device_stop(&gAudio.state.device);
    ma_device_uninit(&gAudio.state.device);
    gAudio.deviceReady = false;
  }
  if (gAudio.decoderReady) {
    stopAndUninitActiveDecoder();
  }

  gAudio.channels = newChannels;
  gAudio.state.channels = gAudio.channels;
  gAudio.radio1938Template.init(kRadioProcessChannels,
                                static_cast<float>(gAudio.sampleRate),
                                gAudio.lpHz,
                                gAudio.noise);
  rebuildRadioPreviewChain(&gAudio.state);

  if (hadTrack) {
    return loadFileAt(gAudio.nowPlaying, resumeFrame, trackIndex);
  }
  return true;
}

void stopPlayback() {
  if (gAudio.audition.active.load()) {
    stopAuditionWorker();
    gAudio.audition.resumeValid = false;
  }
  if (gAudio.deviceReady) {
    ma_device_stop(&gAudio.state.device);
  }
  if (gAudio.decoderReady) {
    stopAndUninitActiveDecoder();
  }
  gAudio.state.framesPlayed.store(0);
  gAudio.state.audioClockFrames.store(0);
  gAudio.state.finished.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingSeekFrames.store(0);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.paused.store(false);
  gAudio.state.hold.store(false);
  gAudio.state.externalStream.store(false);
  gAudio.state.streamQueueEnabled.store(false);
  gAudio.state.streamSerial.store(0);
  gAudio.state.streamBaseValid.store(false);
  gAudio.state.streamBasePtsUs.store(0);
  gAudio.state.streamReadFrames.store(0);
  gAudio.state.streamClockReady.store(false);
  gAudio.state.streamStarved.store(false);
  gAudio.state.streamRb.reset();
  gAudio.state.audioClock.reset(0);
  melodyOfflineStop();
  gAudio.nowPlaying.clear();
  gAudio.trackIndex = 0;
  gAudio.lastInitError.clear();
  gAudio.gmeWarning.clear();
  gAudio.gsfWarning.clear();
  gAudio.vgmWarning.clear();
}
}  // namespace

void audioStreamProcessRadio(float* interleaved, uint32_t frames) {
  audioStreamProcessRadioImpl(interleaved, frames);
}

void audioInit(const AudioPlaybackConfig& config) {
  gAudio.enableAudio = config.enableAudio;
  gAudio.enableRadio = config.enableRadio;
  gAudio.dry = config.dry;
  gAudio.sampleRate = 48000;
  gAudio.baseChannels = config.mono ? 1u : 2u;
  gAudio.channels = gAudio.baseChannels;
  gAudio.lpHz = static_cast<float>(config.bwHz);
  gAudio.noise = static_cast<float>(config.noise);
  gAudio.radioPresetName = config.radioPresetName;

  gAudio.state.channels = gAudio.channels;
  gAudio.state.sampleRate = gAudio.sampleRate;
  gAudio.state.dry = config.dry;
  gAudio.state.useRadio1938.store(config.enableRadio);
  gAudio.radioSettingsPath = config.radioSettingsPath;

  gAudio.radio1938Template.init(kRadioProcessChannels,
                                static_cast<float>(gAudio.sampleRate),
                                gAudio.lpHz,
                                gAudio.noise);
  if (config.enableRadio) {
    applyRadioTogglePreset();
  } else {
    rebuildRadioPreviewChain(&gAudio.state);
  }
}

void audioShutdown() {
  stopPlayback();
  if (gAudio.deviceReady) {
    ma_device_uninit(&gAudio.state.device);
    gAudio.deviceReady = false;
  }
}

bool audioIsEnabled() { return gAudio.enableAudio; }

bool audioIsReady() { return gAudio.decoderReady; }

bool audioStartFile(const std::filesystem::path& file, int trackIndex) {
  return loadFileAt(file, 0, trackIndex);
}

bool audioStartFileAt(const std::filesystem::path& file, double startSec,
                      int trackIndex) {
  if (!std::isfinite(startSec) || startSec <= 0.0) {
    return loadFileAt(file, 0, trackIndex);
  }
  double framesDouble = startSec * static_cast<double>(gAudio.sampleRate);
  int64_t startFrame = static_cast<int64_t>(std::llround(framesDouble));
  if (startFrame < 0) startFrame = 0;
  return loadFileAt(file, static_cast<uint64_t>(startFrame), trackIndex);
}

void audioStop() {
  gAudio.audition.resumeValid = false;
  stopPlayback();
}

bool audioStartStream(uint64_t totalFrames) {
  melodyOfflineStop();
  gAudio.lastInitError.clear();
  gAudio.gmeWarning.clear();
  gAudio.gsfWarning.clear();
  gAudio.vgmWarning.clear();
  gAudio.trackIndex = 0;
  if (gAudio.deviceReady) {
    ma_device_stop(&gAudio.state.device);
  }
  stopAndUninitActiveDecoder();

  const uint32_t rbFrames = std::max<uint32_t>(gAudio.sampleRate, 8192);
  {
    std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
    audioRingBufferInit(&gAudio.state.m4aBuffer, rbFrames, gAudio.channels);
    gAudio.state.m4aInitDone = true;
    gAudio.state.m4aInitOk = true;
    gAudio.state.m4aInitError.clear();
  }
  gAudio.state.streamRb.init(rbFrames, gAudio.channels);
  gAudio.state.streamQueueEnabled.store(true);
  gAudio.state.streamSerial.store(0);
  gAudio.state.streamBaseValid.store(false);
  gAudio.state.streamBasePtsUs.store(0);
  gAudio.state.streamReadFrames.store(0);
  gAudio.state.streamClockReady.store(false);
  gAudio.state.streamStarved.store(false);
  gAudio.state.audioClock.reset(0);
  gAudio.state.m4aThreadRunning.store(false);
  gAudio.state.m4aStop.store(false);
  gAudio.state.m4aAtEnd.store(false);
  gAudio.state.externalStream.store(true);
  gAudio.state.mode.store(AudioMode::Stream, std::memory_order_relaxed);
  gAudio.state.backend = nullptr;
  gAudio.decoderReady = true;
  gAudio.trackIndex = 0;

  gAudio.state.framesPlayed.store(0);
  gAudio.state.audioClockFrames.store(0);
  gAudio.state.callbackCount.store(0);
  gAudio.state.framesRequested.store(0);
  gAudio.state.framesReadTotal.store(0);
  gAudio.state.shortReadCount.store(0);
  gAudio.state.silentFrames.store(0);
  gAudio.state.pausedCallbacks.store(0);
  gAudio.state.lastCallbackFrames.store(0);
  gAudio.state.lastFramesRead.store(0);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingSeekFrames.store(0);
  gAudio.state.finished.store(false);
  gAudio.state.paused.store(false);
  gAudio.state.hold.store(false);
  gAudio.state.audioPaddingFrames.store(0);
  gAudio.state.audioTrailingPaddingFrames.store(0);
  gAudio.state.audioLeadSilenceFrames.store(0);
  gAudio.state.totalFrames.store(totalFrames);

  gAudio.state.channels = gAudio.channels;
  gAudio.state.sampleRate = gAudio.sampleRate;
  rebuildRadioPreviewChain(&gAudio.state);

  if (!ensurePlaybackDeviceRunning()) {
    gAudio.state.streamQueueEnabled.store(false);
    return false;
  }

  // Estimate output latency so the audio clock tracks what is actually audible,
  // not what we just wrote into the callback buffer.
  uint64_t latencyFrames = 0;
  if (gAudio.deviceReady &&
      ma_device_get_state(&gAudio.state.device) !=
          ma_device_state_uninitialized) {
    uint32_t internalRate = gAudio.state.device.playback.internalSampleRate;
    uint64_t internalBuffer = 0;
    uint32_t periodFrames = gAudio.state.device.playback.internalPeriodSizeInFrames;
    uint32_t periods = gAudio.state.device.playback.internalPeriods;
    if (periodFrames > 0 && periods > 0) {
      internalBuffer = static_cast<uint64_t>(periodFrames) *
                       static_cast<uint64_t>(periods);
    }
#ifdef MA_SUPPORT_WASAPI
    if (gAudio.state.device.wasapi.actualBufferSizeInFramesPlayback > 0) {
      internalBuffer = std::max<uint64_t>(
          internalBuffer, gAudio.state.device.wasapi.actualBufferSizeInFramesPlayback);
    }
#endif
    if (internalBuffer > 0) {
      latencyFrames +=
          rescaleFrames(internalBuffer, internalRate, gAudio.state.sampleRate);
    }
    latencyFrames +=
        static_cast<uint64_t>(gAudio.state.device.playback.intermediaryBufferLen);
    uint64_t converterLatency =
        ma_data_converter_get_output_latency(&gAudio.state.device.playback.converter);
    if (converterLatency > 0) {
      latencyFrames +=
          rescaleFrames(converterLatency, internalRate, gAudio.state.sampleRate);
    }
  }
  gAudio.state.deviceDelayFrames = latencyFrames;

  gAudio.nowPlaying.clear();
  return true;
}

void audioStopStream() {
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  stopPlayback();
}

size_t audioStreamBufferedFrames() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return static_cast<size_t>(gAudio.state.streamRb.bufferedFrames());
}

size_t audioStreamCapacityFrames() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return static_cast<size_t>(gAudio.state.streamRb.capFrames);
}

int64_t audioStreamOldestPtsUs() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return AV_NOPTS_VALUE;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return AV_NOPTS_VALUE;
  }

  // Gebruik de metadata wachtrij om de ECHTE tijdstempel van de oudste sample in de buffer te vinden.
  std::lock_guard<std::mutex> lock(gAudio.state.streamMetadataMutex);
  if (gAudio.state.streamMetadata.empty()) {
    return AV_NOPTS_VALUE;
  }

  const auto& md = gAudio.state.streamMetadata.front();
  uint64_t currentRpos = gAudio.state.streamRb.rpos.load(std::memory_order_relaxed);
  
  // Als we al samples hebben gelezen voorbij het startpunt van deze metadata
  if (currentRpos >= md.wpos) {
    uint64_t offset = currentRpos - md.wpos;
    return md.ptsUs + static_cast<int64_t>(offset * 1000000ULL / gAudio.state.sampleRate);
  } else {
    // Dit zou niet mogen gebeuren, maar voor de zekerheid:
    return md.ptsUs;
  }
}

bool audioStreamWriteSamples(const float* interleaved, uint64_t frames,
                             int64_t ptsUs, int serial, bool allowBlock,
                             uint64_t* writtenFrames) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return false;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return false;
  }
  return writeQueuedSamples(&gAudio.state, interleaved, frames, ptsUs, serial,
                            allowBlock, writtenFrames);
}

uint64_t audioStreamDropFrames(uint64_t frames) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return gAudio.state.streamRb.dropSome(frames);
}

void audioStreamSynchronize(int serial, int64_t targetPtsUs) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load() ||
      !gAudio.state.streamQueueEnabled.load()) {
    return;
  }

  // We dwingen een harde synchronisatie.
  // Door de ringbuffer te resetten, zorgen we ervoor dat 'oude' audio direct
  // wordt weggegooid en de klok onmiddellijk op de nieuwe tijd staat.
  gAudio.state.streamRb.reset();
  
  {
    std::lock_guard<std::mutex> lock(gAudio.state.streamMetadataMutex);
    gAudio.state.streamMetadata.clear();
    // We voegen de nieuwe anker-pts toe voor de EERSTE sample die nu geschreven gaat worden (wpos=0).
    gAudio.state.streamMetadata.push_back({
        0,
        targetPtsUs,
        serial
    });
  }

  gAudio.state.streamDiscardPtsUs.store(targetPtsUs, std::memory_order_relaxed);
  gAudio.state.streamBasePtsUs.store(targetPtsUs, std::memory_order_relaxed);
  gAudio.state.audioClock.set(targetPtsUs, nowUs(), serial);
  gAudio.state.streamClockReady.store(true, std::memory_order_relaxed);
  gAudio.state.decodeCv.notify_all();
}

void audioStreamSyncClockOnly(int serial, int64_t targetPtsUs) {
  // Soft sync: only adjust the clock, don't reset ringbuffer or drop audio
  // Used when video PTS is repaired to prevent audio clock from becoming invalid
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load() ||
      !gAudio.state.streamQueueEnabled.load()) {
    return;
  }
  
  // Just resync the clock without touching the ringbuffer
  // This keeps audio playing smoothly while video catches up
  gAudio.state.audioClock.sync_to_pts(targetPtsUs, nowUs(), serial);
}

void audioStreamSetBase(int serial, int64_t ptsUs) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return;
  }
  if (serial != gAudio.state.streamSerial.load(std::memory_order_relaxed)) {
    return;
  }
  gAudio.state.streamBasePtsUs.store(ptsUs, std::memory_order_relaxed);
  gAudio.state.streamBaseValid.store(true, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.audioClock.reset(serial);
}

void audioStreamDiscardUntil(int64_t ptsUs) {
  gAudio.state.streamDiscardPtsUs.store(ptsUs, std::memory_order_relaxed);
}

void audioStreamSetEnd(bool atEnd) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  gAudio.state.m4aAtEnd.store(atEnd);
}

void audioStreamReset(uint64_t framePos) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  gAudio.state.streamRb.reset();
  gAudio.state.framesPlayed.store(framePos);
  gAudio.state.audioClockFrames.store(framePos);
  gAudio.state.finished.store(false);
  gAudio.state.m4aAtEnd.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingSeekFrames.store(static_cast<int64_t>(framePos));
  gAudio.state.audioPrimed.store(false);
  gAudio.state.streamBaseValid.store(false, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.audioClock.reset(
      gAudio.state.streamSerial.load(std::memory_order_relaxed));
  {
    std::lock_guard<std::mutex> lock(gAudio.state.streamMetadataMutex);
    gAudio.state.streamMetadata.clear();
  }
  gAudio.state.decodeCv.notify_all();
}

void audioStreamFlushSerial(int serial) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  gAudio.state.streamSerial.store(serial, std::memory_order_relaxed);
  gAudio.state.streamRb.reset();
  gAudio.state.streamBaseValid.store(false, std::memory_order_relaxed);
  gAudio.state.streamBasePtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.m4aAtEnd.store(false);
  gAudio.state.finished.store(false);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.audioClock.reset(serial);
  {
    std::lock_guard<std::mutex> lock(gAudio.state.streamMetadataMutex);
    gAudio.state.streamMetadata.clear();
  }
  gAudio.state.decodeCv.notify_all();
}

int audioStreamSerial() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return gAudio.state.streamSerial.load(std::memory_order_relaxed);
}

int64_t audioStreamClockUs(int64_t nowUs) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  if (!gAudio.state.audioClock.is_valid()) {
    return 0;
  }
  
  // The audioClock is now updated in dataCallback with hardware latency compensation included.
  return gAudio.state.audioClock.get(nowUs);
}

int64_t audioStreamClockLastUpdatedUs() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return gAudio.state.audioClock.last_updated_us.load(std::memory_order_relaxed);
}

bool audioStreamStarved() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return false;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return false;
  }
  return gAudio.state.streamStarved.load(std::memory_order_relaxed);
}

bool audioStreamClockReady() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return false;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return false;
  }
  return gAudio.state.audioClock.is_valid();
}

uint64_t audioStreamWaitForUpdate(uint64_t lastCounter, int timeoutMs) {
  std::unique_lock<std::mutex> lock(gAudio.state.streamMetadataMutex);
  gAudio.state.streamUpdateCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
    return gAudio.state.streamUpdateCounter.load(std::memory_order_acquire) != lastCounter;
  });
  return gAudio.state.streamUpdateCounter.load(std::memory_order_acquire);
}

uint64_t audioStreamUpdateCounter() {
  return gAudio.state.streamUpdateCounter.load(std::memory_order_acquire);
}

std::filesystem::path audioGetNowPlaying() { return gAudio.nowPlaying; }

int audioGetTrackIndex() {
  if (!gAudio.decoderReady) return -1;
  const AudioBackendHandlers* backend = gAudio.state.backend;
  if (!backend || !backend->supportsTrackIndex) return -1;
  return gAudio.trackIndex;
}

double audioGetTimeSec() {
  if (!gAudio.decoderReady) {
    return 0.0;
  }
  if (!gAudio.state.audioPrimed.load()) {
    return 0.0;
  }
  int64_t frames = static_cast<int64_t>(gAudio.state.audioClockFrames.load());
  uint64_t latencyFrames = 0;
  if (gAudio.deviceReady &&
      ma_device_get_state(&gAudio.state.device) !=
          ma_device_state_uninitialized) {
    uint32_t internalRate = gAudio.state.device.playback.internalSampleRate;
    uint64_t internalBuffer = 0;
    uint32_t periodFrames = gAudio.state.device.playback.internalPeriodSizeInFrames;
    uint32_t periods = gAudio.state.device.playback.internalPeriods;
    if (periodFrames > 0 && periods > 0) {
      internalBuffer = static_cast<uint64_t>(periodFrames) *
                       static_cast<uint64_t>(periods);
    }
#ifdef MA_SUPPORT_WASAPI
    if (gAudio.state.device.wasapi.actualBufferSizeInFramesPlayback > 0) {
      internalBuffer = std::max<uint64_t>(
          internalBuffer, gAudio.state.device.wasapi.actualBufferSizeInFramesPlayback);
    }
#endif
    if (internalBuffer > 0) {
      latencyFrames +=
          rescaleFrames(internalBuffer, internalRate, gAudio.sampleRate);
    }
    latencyFrames +=
        static_cast<uint64_t>(gAudio.state.device.playback.intermediaryBufferLen);
    uint64_t converterLatency =
        ma_data_converter_get_output_latency(&gAudio.state.device.playback.converter);
    if (converterLatency > 0) {
      latencyFrames +=
          rescaleFrames(converterLatency, internalRate, gAudio.sampleRate);
    }
  }
  frames -= static_cast<int64_t>(latencyFrames);
  if (frames < 0) {
    frames = 0;
  }
  double timeSec = static_cast<double>(frames) / gAudio.sampleRate;
  uint64_t totalFrames = gAudio.state.totalFrames.load();
  if (totalFrames > 0) {
    double totalSec = static_cast<double>(totalFrames) / gAudio.sampleRate;
    if (gAudio.state.finished.load() || timeSec > totalSec) {
      timeSec = totalSec;
    }
  }
  return timeSec;
}

double audioGetTotalSec() {
  if (!gAudio.decoderReady) {
    return -1.0;
  }
  uint64_t total = gAudio.state.totalFrames.load();
  if (total == 0) return -1.0;
  return static_cast<double>(total) / gAudio.sampleRate;
}

bool audioIsSeeking() {
  if (!gAudio.decoderReady) {
    return false;
  }
  return gAudio.state.seekRequested.load();
}

double audioGetSeekTargetSec() {
  if (!gAudio.decoderReady) {
    return -1.0;
  }
  uint64_t total = gAudio.state.totalFrames.load();
  if (total == 0) return -1.0;
  int64_t target = gAudio.state.pendingSeekFrames.load();
  if (target < 0) target = 0;
  if (static_cast<uint64_t>(target) > total) {
    target = static_cast<int64_t>(total);
  }
  return static_cast<double>(target) / gAudio.sampleRate;
}

bool audioIsPrimed() {
  if (!gAudio.decoderReady) return false;
  return gAudio.state.audioPrimed.load();
}

bool audioIsPaused() {
  if (!gAudio.decoderReady) return false;
  return gAudio.state.paused.load();
}

bool audioIsFinished() {
  if (!gAudio.decoderReady) return false;
  return gAudio.state.finished.load();
}

bool audioIsRadioEnabled() { return gAudio.state.useRadio1938.load(); }

bool audioIsHolding() { return gAudio.state.hold.load(); }

AudioPerfStats audioGetPerfStats() {
  AudioPerfStats stats{};
  if (!gAudio.decoderReady) {
    return stats;
  }
  stats.callbacks = gAudio.state.callbackCount.load(std::memory_order_relaxed);
  stats.framesRequested =
      gAudio.state.framesRequested.load(std::memory_order_relaxed);
  stats.framesRead = gAudio.state.framesReadTotal.load(std::memory_order_relaxed);
  stats.shortReads = gAudio.state.shortReadCount.load(std::memory_order_relaxed);
  stats.silentFrames = gAudio.state.silentFrames.load(std::memory_order_relaxed);
  stats.lastCallbackFrames =
      gAudio.state.lastCallbackFrames.load(std::memory_order_relaxed);
  stats.lastFramesRead =
      gAudio.state.lastFramesRead.load(std::memory_order_relaxed);
  stats.sampleRate = gAudio.state.sampleRate;
  stats.channels = gAudio.state.channels;
  const AudioMode mode = currentAudioMode();
  stats.usingFfmpeg = mode == AudioMode::M4a || mode == AudioMode::Ffmpeg;
  if (gAudio.deviceReady &&
      ma_device_get_state(&gAudio.state.device) !=
          ma_device_state_uninitialized) {
    stats.periodFrames = gAudio.state.device.playback.internalPeriodSizeInFrames;
    stats.periods = gAudio.state.device.playback.internalPeriods;
    stats.bufferFrames = stats.periodFrames * stats.periods;
  }
  return stats;
}

void audioTogglePause() {
  if (!gAudio.decoderReady) return;
  gAudio.state.paused.store(!gAudio.state.paused.load());
}

void audioSeekBy(int direction) {
  if (!gAudio.decoderReady) return;
  int64_t deltaFrames = static_cast<int64_t>(direction) * 5 * gAudio.sampleRate;
  int64_t current = static_cast<int64_t>(gAudio.state.audioClockFrames.load());
  int64_t target = current + deltaFrames;
  if (target < 0) target = 0;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total > 0 && target > static_cast<int64_t>(total)) {
    target = static_cast<int64_t>(total);
  }
  gAudio.state.pendingSeekFrames.store(target);
  gAudio.state.seekRequested.store(true);
  gAudio.state.finished.store(false);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.m4aCv.notify_all();
  gAudio.state.decodeCv.notify_all();
}

void audioSeekToRatio(double ratio) {
  if (!gAudio.decoderReady) return;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total == 0) return;
  ratio = std::clamp(ratio, 0.0, 1.0);
  int64_t target = static_cast<int64_t>(ratio * static_cast<double>(total));
  gAudio.state.pendingSeekFrames.store(target);
  gAudio.state.seekRequested.store(true);
  gAudio.state.finished.store(false);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.m4aCv.notify_all();
  gAudio.state.decodeCv.notify_all();
}

void audioSeekToSec(double sec) {
  if (!gAudio.decoderReady) return;
  if (!std::isfinite(sec)) return;
  int64_t target =
      static_cast<int64_t>(std::llround(sec * gAudio.sampleRate));
  if (target < 0) target = 0;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total > 0 && static_cast<uint64_t>(target) > total) {
    target = static_cast<int64_t>(total);
  }
  gAudio.state.pendingSeekFrames.store(target);
  gAudio.state.seekRequested.store(true);
  gAudio.state.finished.store(false);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.m4aCv.notify_all();
  gAudio.state.decodeCv.notify_all();
}

void audioToggleRadio() {
  bool next = !gAudio.state.useRadio1938.load();
  gAudio.state.useRadio1938.store(next);
  if (next) {
    applyRadioTogglePreset();
  }
}

static void reloadKssWithOptions() {
  if (!gAudio.decoderReady || !isAudioMode(AudioMode::Kss)) return;
  uint64_t resumeFrame = gAudio.state.framesPlayed.load();
  bool wasPaused = gAudio.state.paused.load();
  if (loadFileAt(gAudio.nowPlaying, resumeFrame, gAudio.trackIndex)) {
    if (wasPaused) {
      gAudio.state.paused.store(true);
    }
  }
}

static void reloadNsfWithOptions();

void audioToggle50Hz() {
  if (!audioSupports50HzToggle()) {
    return;
  }
  switch (currentAudioMode()) {
    case AudioMode::Vgm: {
      gAudio.vgmOptions.playbackHz =
          (gAudio.vgmOptions.playbackHz == VgmPlaybackHz::Hz50)
              ? VgmPlaybackHz::Hz60
              : VgmPlaybackHz::Hz50;
      gAudio.state.vgm.applyOptions(gAudio.vgmOptions);
      uint64_t totalFrames = 0;
      if (gAudio.state.vgm.getTotalFrames(&totalFrames)) {
        gAudio.state.totalFrames.store(totalFrames);
      } else {
        gAudio.state.totalFrames.store(0);
      }
      return;
    }
    case AudioMode::Gme:
      gAudio.nsfOptions.tempoMode =
          (gAudio.nsfOptions.tempoMode == NsfTempoMode::Pal50)
              ? NsfTempoMode::Normal
              : NsfTempoMode::Pal50;
      reloadNsfWithOptions();
      return;
    case AudioMode::Kss:
      gAudio.kssOptions.force50Hz = !gAudio.kssOptions.force50Hz;
      reloadKssWithOptions();
      return;
    default:
      return;
  }
}

void audioSetHold(bool hold) { gAudio.state.hold.store(hold); }

void audioAdjustVolume(float delta) {
  float current = gAudio.state.volume.load(std::memory_order_relaxed);
  float next = std::clamp(current + delta, 0.0f, 4.0f);
  gAudio.state.volume.store(next, std::memory_order_relaxed);
}

float audioGetVolume() {
  return gAudio.state.volume.load(std::memory_order_relaxed);
}

float audioGetPeak() {
  return gAudio.state.peak.load(std::memory_order_relaxed);
}

bool audioHasClipAlert() {
  return gAudio.state.clipAlertUntilUs.load(std::memory_order_relaxed) >
         nowUs();
}

AudioMelodyInfo audioGetMelodyInfo() {
  if (!gAudio.decoderReady) {
    return AudioMelodyInfo{};
  }
  MelodyOfflineAnalysisState analysisState = melodyOfflineGetState();
  if (!analysisState.running && !analysisState.ready) {
    return {};
  }
  const MelodyOfflineFrame frame =
      melodyOfflineGetFrame(audioGetTimeSec());
  AudioMelodyInfo info{};
  info.frequencyHz = frame.frequencyHz;
  info.confidence = std::clamp(frame.confidence, 0.0f, 1.0f);
  info.midiNote = frame.midiNote;
  if (!std::isfinite(info.frequencyHz) || !std::isfinite(info.confidence) ||
      info.frequencyHz <= 0.0f || info.midiNote < 0 || info.midiNote > 127) {
    info.frequencyHz = 0.0f;
    info.midiNote = -1;
  }
  return info;
}

AudioMelodyAnalysisState audioGetMelodyAnalysisState() {
  const MelodyOfflineAnalysisState state = melodyOfflineGetState();
  AudioMelodyAnalysisState result;
  result.ready = state.ready;
  result.running = state.running;
  result.progress = state.progress;
  result.frameCount = state.frameCount;
  result.error = state.error;
  return result;
}

bool audioAnalyzeFileToMelodyFile(const std::filesystem::path& file,
                                  int trackIndex,
                                  const std::filesystem::path& outputFile,
                                  const std::function<void(float)>& progressCallback,
                                  std::string* error) {
  if (file.empty() || !std::filesystem::exists(file)) {
    if (error) *error = "Input file not found.";
    return false;
  }
  uint32_t analysisSampleRate = std::max<uint32_t>(1u, gAudio.sampleRate);
  uint32_t analysisChannels =
      std::clamp<uint32_t>(std::max<uint32_t>(1u, gAudio.baseChannels), 1u, 2u);
  return melodyOfflineAnalyzeToFile(
      file, trackIndex, analysisSampleRate, analysisChannels, 0,
      gAudio.kssOptions, gAudio.nsfOptions, gAudio.vgmOptions,
      gAudio.vgmDeviceOverrides, outputFile, progressCallback, error);
}

std::string audioGetWarning() {
  std::string warning = warningForBackend(gAudio.state.backend);
  if (!warning.empty()) return warning;
  return gAudio.lastInitError;
}

bool audioIs50HzEnabled() {
  switch (currentAudioMode()) {
    case AudioMode::Vgm:
      return gAudio.vgmOptions.playbackHz == VgmPlaybackHz::Hz50;
    case AudioMode::Gme:
      return gAudio.nsfOptions.tempoMode == NsfTempoMode::Pal50;
    case AudioMode::Kss:
      return gAudio.kssOptions.force50Hz;
    default:
      return false;
  }
}

bool audioSupports50HzToggle() {
  const AudioMode mode = currentAudioMode();
  return mode == AudioMode::Kss || mode == AudioMode::Gme ||
         mode == AudioMode::Vgm;
}

KssPlaybackOptions audioGetKssOptionState() {
  return gAudio.kssOptions;
}

static bool toKssDevice(KssInstrumentDevice device, KSS_DEVICE* out) {
  if (!out) return false;
  switch (device) {
    case KssInstrumentDevice::Psg:
      *out = KSS_DEVICE_PSG;
      return true;
    case KssInstrumentDevice::Scc:
      *out = KSS_DEVICE_SCC;
      return true;
    case KssInstrumentDevice::Opll:
      *out = KSS_DEVICE_OPLL;
      return true;
    case KssInstrumentDevice::None:
    default:
      break;
  }
  return false;
}

bool audioGetKssInstrumentRegs(KssInstrumentDevice device,
                               std::vector<uint8_t>* out) {
  if (!out) return false;
  if (!isAudioMode(AudioMode::Kss)) return false;
  if (!gAudio.state.kss.active()) return false;
  KSS_DEVICE kssDevice{};
  if (!toKssDevice(device, &kssDevice)) return false;
  return gAudio.state.kss.readDeviceRegs(kssDevice, out);
}

bool audioSetKssInstrumentPreview(KssInstrumentDevice device, int channel) {
  bool changed = false;
  int maxChannels = 0;
  switch (device) {
    case KssInstrumentDevice::Psg:
      maxChannels = 3;
      break;
    case KssInstrumentDevice::Scc:
      maxChannels = 5;
      break;
    case KssInstrumentDevice::Opll:
      maxChannels = 9;
      break;
    case KssInstrumentDevice::None:
    default:
      break;
  }

  if (device == KssInstrumentDevice::None || channel < 0 ||
      (maxChannels > 0 && channel >= maxChannels)) {
    if (gAudio.kssOptions.instrumentDevice != KssInstrumentDevice::None ||
        gAudio.kssOptions.instrumentChannel != -1) {
      gAudio.kssOptions.instrumentDevice = KssInstrumentDevice::None;
      gAudio.kssOptions.instrumentChannel = -1;
      changed = true;
    }
  } else if (gAudio.kssOptions.instrumentDevice != device ||
             gAudio.kssOptions.instrumentChannel != channel) {
    gAudio.kssOptions.instrumentDevice = device;
    gAudio.kssOptions.instrumentChannel = channel;
    changed = true;
  }

  if (changed) {
    reloadKssWithOptions();
  }
  return changed;
}

bool audioGetKssInstrumentAuditionState(KssInstrumentDevice* device,
                                        uint32_t* hash) {
  if (device) *device = gAudio.audition.device;
  if (hash) *hash = gAudio.audition.hash;
  return gAudio.audition.active.load();
}

bool audioStartKssInstrumentAudition(const KssInstrumentProfile& profile) {
  if (profile.device != KssInstrumentDevice::Psg &&
      profile.device != KssInstrumentDevice::Scc) {
    return false;
  }

  AuditionTone tone;
  bool ok = false;
  if (profile.device == KssInstrumentDevice::Psg) {
    ok = buildPsgAuditionTone(profile, gAudio.sampleRate,
                              gAudio.kssOptions.psgType, &tone);
  } else if (profile.device == KssInstrumentDevice::Scc) {
    ok = buildSccAuditionTone(profile, gAudio.sampleRate,
                              gAudio.kssOptions.sccType,
                              gAudio.kssOptions.sccQuality, &tone);
  }
  if (!ok) return false;

  if (gAudio.audition.active.load()) {
    stopAuditionWorker();
    if (gAudio.state.externalStream.load()) {
      audioStreamReset(0);
    }
  } else {
    gAudio.audition.resumeValid =
        gAudio.decoderReady && !gAudio.nowPlaying.empty() &&
        !gAudio.state.externalStream.load();
    if (gAudio.audition.resumeValid) {
      gAudio.audition.resumeFile = gAudio.nowPlaying;
      gAudio.audition.resumeFrame = gAudio.state.framesPlayed.load();
      gAudio.audition.resumePaused = gAudio.state.paused.load();
      gAudio.audition.resumeTrackIndex = gAudio.trackIndex;
    }
    if (!audioStartStream(0)) {
      if (gAudio.audition.resumeValid) {
        loadFileAt(gAudio.audition.resumeFile, gAudio.audition.resumeFrame,
                   gAudio.audition.resumeTrackIndex);
        if (gAudio.audition.resumePaused) {
          gAudio.state.paused.store(true);
        }
      }
      gAudio.audition.resumeValid = false;
      return false;
    }
  }

  gAudio.audition.device = profile.device;
  gAudio.audition.hash = profile.hash;
  startAuditionWorker(std::move(tone));
  return true;
}

bool audioStopKssInstrumentAudition() {
  if (!gAudio.audition.active.load()) return false;
  stopAuditionWorker();
  gAudio.audition.device = KssInstrumentDevice::None;
  gAudio.audition.hash = 0;

  if (gAudio.audition.resumeValid) {
    bool resumed = loadFileAt(gAudio.audition.resumeFile,
                              gAudio.audition.resumeFrame,
                              gAudio.audition.resumeTrackIndex);
    if (resumed && gAudio.audition.resumePaused) {
      gAudio.state.paused.store(true);
    }
    gAudio.audition.resumeValid = false;
    return resumed;
  }
  audioStopStream();
  gAudio.audition.resumeValid = false;
  return true;
}

bool audioScanKssInstruments(const std::filesystem::path& file, int trackIndex,
                             std::vector<KssInstrumentProfile>* out,
                             std::string* error) {
  if (!out) return false;
  out->clear();
  if (!isKssExt(file)) return false;

  KssPlaybackOptions options = gAudio.kssOptions;
  options.instrumentDevice = KssInstrumentDevice::None;
  options.instrumentChannel = -1;

  KssAudioDecoder decoder;
  if (!decoder.init(file, 1, gAudio.sampleRate, error, trackIndex, options)) {
    return false;
  }

  uint64_t totalFrames = 0;
  decoder.getTotalFrames(&totalFrames);
  if (totalFrames == 0) {
    totalFrames = static_cast<uint64_t>(gAudio.sampleRate) * 150;
  }

  std::unordered_map<std::string, size_t> seen;

  auto addPsgInstrument = [&](const uint8_t* key, size_t keySize,
                              const uint8_t* data, size_t dataSize,
                              uint8_t volume, bool envUsed) {
    std::string mapKey;
    mapKey.reserve(keySize + 1);
    mapKey.push_back('P');
    mapKey.append(reinterpret_cast<const char*>(key), keySize);
    auto it = seen.find(mapKey);
    if (it != seen.end()) {
      KssInstrumentProfile& existing = (*out)[it->second];
      if (dataSize >= 6 && existing.data.size() >= 6) {
        bool existingEnv = (existing.data[1] & 0x10) != 0;
        if (envUsed && existingEnv) {
          uint16_t envPeriod =
              static_cast<uint16_t>(data[2]) |
              (static_cast<uint16_t>(data[3]) << 8);
          uint16_t existingPeriod =
              static_cast<uint16_t>(existing.data[2]) |
              (static_cast<uint16_t>(existing.data[3]) << 8);
          if (envPeriod != 0 && existingPeriod == 0) {
            existing.data[2] = data[2];
            existing.data[3] = data[3];
          }
        }
        if (!envUsed && !existingEnv) {
          uint8_t existingVolume = existing.data[1] & 0x0f;
          if ((volume & 0x0f) > existingVolume) {
            existing.data[1] = static_cast<uint8_t>(volume & 0x0f);
          }
        }
        bool noiseEnabled = (data[0] & 0x2) == 0;
        if (noiseEnabled && existing.data[5] == 0 && data[5] != 0) {
          existing.data[5] = data[5];
        }
      }
      if (volume > existing.volume) existing.volume = volume;
      return;
    }

    KssInstrumentProfile profile;
    profile.device = KssInstrumentDevice::Psg;
    profile.data.assign(data, data + dataSize);
    profile.hash = fnv1a32(
        reinterpret_cast<const uint8_t*>(mapKey.data()), mapKey.size());
    profile.volume = volume;
    seen.emplace(std::move(mapKey), out->size());
    out->push_back(std::move(profile));
  };

  auto addSccInstrument = [&](const uint8_t* wave, size_t waveSize,
                              uint8_t volume) {
    std::string mapKey;
    mapKey.reserve(waveSize + 1);
    mapKey.push_back('S');
    mapKey.append(reinterpret_cast<const char*>(wave), waveSize);
    auto it = seen.find(mapKey);
    if (it != seen.end()) {
      KssInstrumentProfile& existing = (*out)[it->second];
      if (volume > existing.volume) existing.volume = volume;
      return;
    }

    KssInstrumentProfile profile;
    profile.device = KssInstrumentDevice::Scc;
    profile.data.assign(wave, wave + waveSize);
    profile.hash = fnv1a32(profile.data.data(), profile.data.size());
    profile.volume = volume;
    seen.emplace(std::move(mapKey), out->size());
    out->push_back(std::move(profile));
  };

  auto scanRegs = [&]() {
    std::vector<uint8_t> psgRegs;
    if (decoder.readDeviceRegs(KSS_DEVICE_PSG, &psgRegs) &&
        psgRegs.size() >= 14) {
      uint8_t mixer = psgRegs[7];
      for (int ch = 0; ch < 3; ++ch) {
        uint8_t toneDisable = (mixer >> ch) & 0x1;
        uint8_t noiseDisable = (mixer >> (ch + 3)) & 0x1;
        bool active = (toneDisable == 0) || (noiseDisable == 0);
        uint8_t volReg = psgRegs[static_cast<size_t>(8 + ch)];
        bool env = (volReg & 0x10) != 0;
        if (env && psgRegs[11] == 0 && psgRegs[12] == 0) {
          env = false;
        }
        if (!active) continue;
        if ((volReg & 0x0f) == 0 && !env) continue;

        uint8_t mixKey = static_cast<uint8_t>(
            (toneDisable ? 1 : 0) | (noiseDisable ? 2 : 0));
        uint8_t envShape = static_cast<uint8_t>(psgRegs[13] & 0x0f);
        uint8_t envKey =
            static_cast<uint8_t>((env ? 0x10 : 0x00) | (env ? envShape : 0));
        uint8_t noiseBucket = 0xFF;
        if (noiseDisable == 0) {
          noiseBucket = static_cast<uint8_t>(
              (psgRegs[6] & 0x1f) / 4);
        }
        uint8_t key[3] = {mixKey, envKey, noiseBucket};
        uint8_t data[6] = {
            mixKey,
            static_cast<uint8_t>((env ? 0x10 : 0x00) | (volReg & 0x0f)),
            psgRegs[11],
            psgRegs[12],
            envShape,
            static_cast<uint8_t>(psgRegs[6] & 0x1f),
        };
        addPsgInstrument(key, sizeof(key), data, sizeof(data),
                         static_cast<uint8_t>(volReg & 0x0f), env);
      }
    }

    std::vector<uint8_t> sccRegs;
    if (decoder.readDeviceRegs(KSS_DEVICE_SCC, &sccRegs) &&
        sccRegs.size() >= 0xE0) {
      for (int ch = 0; ch < 5; ++ch) {
        const uint8_t* wave = sccRegs.data() + ch * 32;
        bool allZero = true;
        for (int i = 0; i < 32; ++i) {
          if (wave[i] != 0) {
            allZero = false;
            break;
          }
        }
        if (allZero) continue;
        uint8_t volume =
            sccRegs[0xD0 + static_cast<size_t>(ch)] & 0x0f;
        addSccInstrument(wave, 32, volume);
      }
    }
  };

  scanRegs();
  const uint32_t chunkFrames = 2048;
  std::vector<float> buffer(chunkFrames);
  uint64_t processed = 0;
  while (processed < totalFrames) {
    uint32_t toRead = static_cast<uint32_t>(
        std::min<uint64_t>(chunkFrames, totalFrames - processed));
    uint64_t read = 0;
    if (!decoder.readFrames(buffer.data(), toRead, &read) || read == 0) {
      break;
    }
    processed += read;
    scanRegs();
  }
  return true;
}

bool audioScanVgmMetadata(const std::filesystem::path& file,
                          std::vector<VgmMetadataEntry>* out,
                          std::string* error) {
  if (!out) return false;
  out->clear();
  if (!isVgmExt(file)) return false;
  return vgmReadMetadata(file, out, error);
}

bool audioScanVgmDevices(const std::filesystem::path& file,
                         std::vector<VgmDeviceInfo>* out,
                         std::string* error) {
  if (!out) return false;
  out->clear();
  if (!isVgmExt(file)) return false;

  VgmAudioDecoder decoder;
  uint32_t channels = gAudio.channels == 0 ? 2 : gAudio.channels;
  if (!decoder.init(file, channels, gAudio.sampleRate, error)) {
    return false;
  }
  if (!decoder.getDevices(out)) {
    if (error) *error = "Failed to scan VGM devices.";
    return false;
  }

  gAudio.vgmDevicesFile = file;
  gAudio.vgmDevices = *out;
  gAudio.vgmDeviceDefaults.clear();
  for (const auto& device : *out) {
    VgmDeviceOptions options{};
    if (decoder.getDeviceOptions(device.id, &options)) {
      gAudio.vgmDeviceDefaults[device.id] = options;
    }
  }
  return true;
}

bool audioAdjustKssOption(KssOptionId id, int direction) {
  if (direction == 0) return false;
  bool changed = false;
  switch (id) {
    case KssOptionId::Force50Hz:
      gAudio.kssOptions.force50Hz = !gAudio.kssOptions.force50Hz;
      changed = true;
      break;
    case KssOptionId::SccType: {
      int next = static_cast<int>(gAudio.kssOptions.sccType) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      gAudio.kssOptions.sccType = static_cast<KssSccType>(next);
      changed = true;
      break;
    }
    case KssOptionId::PsgType: {
      int next = static_cast<int>(gAudio.kssOptions.psgType) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      gAudio.kssOptions.psgType = static_cast<KssPsgType>(next);
      changed = true;
      break;
    }
    case KssOptionId::OpllType: {
      int next = static_cast<int>(gAudio.kssOptions.opllType) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      gAudio.kssOptions.opllType = static_cast<KssOpllType>(next);
      changed = true;
      break;
    }
    case KssOptionId::PsgQuality: {
      int next = static_cast<int>(gAudio.kssOptions.psgQuality) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      gAudio.kssOptions.psgQuality = static_cast<KssQuality>(next);
      changed = true;
      break;
    }
    case KssOptionId::SccQuality: {
      int next = static_cast<int>(gAudio.kssOptions.sccQuality) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      gAudio.kssOptions.sccQuality = static_cast<KssQuality>(next);
      changed = true;
      break;
    }
    case KssOptionId::OpllStereo:
      gAudio.kssOptions.opllStereo = !gAudio.kssOptions.opllStereo;
      changed = true;
      break;
    case KssOptionId::MutePsg:
      gAudio.kssOptions.mutePsg = !gAudio.kssOptions.mutePsg;
      changed = true;
      break;
    case KssOptionId::MuteScc:
      gAudio.kssOptions.muteScc = !gAudio.kssOptions.muteScc;
      changed = true;
      break;
    case KssOptionId::MuteOpll:
      gAudio.kssOptions.muteOpll = !gAudio.kssOptions.muteOpll;
      changed = true;
      break;
    default:
      break;
  }
  if (changed) {
    reloadKssWithOptions();
  }
  return changed;
}

NsfPlaybackOptions audioGetNsfOptionState() {
  return gAudio.nsfOptions;
}

static void reloadNsfWithOptions() {
  if (!gAudio.decoderReady || !isAudioMode(AudioMode::Gme)) return;
  if (!isGmeExt(gAudio.nowPlaying)) return;
  uint64_t resumeFrame = gAudio.state.framesPlayed.load();
  bool wasPaused = gAudio.state.paused.load();
  if (loadFileAt(gAudio.nowPlaying, resumeFrame, gAudio.trackIndex)) {
    if (wasPaused) {
      gAudio.state.paused.store(true);
    }
  }
}

bool audioAdjustNsfOption(NsfOptionId id, int direction) {
  if (direction == 0) return false;
  bool changed = false;
  switch (id) {
    case NsfOptionId::EqPreset: {
      int next = static_cast<int>(gAudio.nsfOptions.eqPreset) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 1;
      if (next > 1) next = 0;
      gAudio.nsfOptions.eqPreset = static_cast<NsfEqPreset>(next);
      changed = true;
      break;
    }
    case NsfOptionId::StereoDepth: {
      int next = static_cast<int>(gAudio.nsfOptions.stereoDepth) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      gAudio.nsfOptions.stereoDepth = static_cast<NsfStereoDepth>(next);
      changed = true;
      break;
    }
    case NsfOptionId::IgnoreSilence:
      gAudio.nsfOptions.ignoreSilence = !gAudio.nsfOptions.ignoreSilence;
      changed = true;
      break;
    case NsfOptionId::TempoMode:
      gAudio.nsfOptions.tempoMode =
          (gAudio.nsfOptions.tempoMode == NsfTempoMode::Pal50)
              ? NsfTempoMode::Normal
              : NsfTempoMode::Pal50;
      changed = true;
      break;
    default:
      break;
  }
  if (changed) {
    reloadNsfWithOptions();
  }
  return changed;
}

VgmPlaybackOptions audioGetVgmOptionState() {
  return gAudio.vgmOptions;
}

bool audioGetVgmDeviceOptions(uint32_t deviceId, VgmDeviceOptions* out) {
  if (!out) return false;
  if (isAudioMode(AudioMode::Vgm)) {
    if (gAudio.state.vgm.getDeviceOptions(deviceId, out)) {
      return true;
    }
  }
  auto it = gAudio.vgmDeviceDefaults.find(deviceId);
  if (it == gAudio.vgmDeviceDefaults.end()) return false;
  *out = it->second;
  auto overrideIt = gAudio.vgmDeviceOverrides.find(deviceId);
  if (overrideIt != gAudio.vgmDeviceOverrides.end()) {
    *out = overrideIt->second;
  }
  return true;
}

static void reloadVgmWithOptions() {
  if (!gAudio.decoderReady || !isAudioMode(AudioMode::Vgm)) return;
  if (!isVgmExt(gAudio.nowPlaying)) return;
  uint64_t resumeFrame = gAudio.state.framesPlayed.load();
  bool wasPaused = gAudio.state.paused.load();
  if (loadFileAt(gAudio.nowPlaying, resumeFrame, gAudio.trackIndex)) {
    if (wasPaused) {
      gAudio.state.paused.store(true);
    }
  }
}

bool audioAdjustVgmOption(VgmOptionId id, int direction) {
  if (direction == 0) return false;
  bool changed = false;
  switch (id) {
    case VgmOptionId::PlaybackHz: {
      int next = static_cast<int>(gAudio.vgmOptions.playbackHz) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      gAudio.vgmOptions.playbackHz = static_cast<VgmPlaybackHz>(next);
      changed = true;
      break;
    }
    case VgmOptionId::Speed: {
      int next = gAudio.vgmOptions.speedStep + (direction > 0 ? 1 : -1);
      if (next < 0) next = kVgmSpeedStepCount - 1;
      if (next >= kVgmSpeedStepCount) next = 0;
      gAudio.vgmOptions.speedStep = next;
      changed = true;
      break;
    }
    case VgmOptionId::LoopCount: {
      int idx = findIndex(kVgmLoopSteps,
                          sizeof(kVgmLoopSteps) / sizeof(kVgmLoopSteps[0]),
                          gAudio.vgmOptions.loopCount);
      idx = advanceIndex(
          idx,
          static_cast<int>(sizeof(kVgmLoopSteps) / sizeof(kVgmLoopSteps[0])),
          direction);
      gAudio.vgmOptions.loopCount = kVgmLoopSteps[idx];
      changed = true;
      break;
    }
    case VgmOptionId::FadeLength: {
      int idx = findIndex(
          kVgmFadeStepsMs,
          sizeof(kVgmFadeStepsMs) / sizeof(kVgmFadeStepsMs[0]),
          gAudio.vgmOptions.fadeMs);
      idx = advanceIndex(
          idx,
          static_cast<int>(sizeof(kVgmFadeStepsMs) /
                           sizeof(kVgmFadeStepsMs[0])),
          direction);
      gAudio.vgmOptions.fadeMs = kVgmFadeStepsMs[idx];
      changed = true;
      break;
    }
    case VgmOptionId::EndSilence: {
      int idx = findIndex(
          kVgmEndSilenceStepsMs,
          sizeof(kVgmEndSilenceStepsMs) / sizeof(kVgmEndSilenceStepsMs[0]),
          gAudio.vgmOptions.endSilenceMs);
      idx = advanceIndex(
          idx,
          static_cast<int>(sizeof(kVgmEndSilenceStepsMs) /
                           sizeof(kVgmEndSilenceStepsMs[0])),
          direction);
      gAudio.vgmOptions.endSilenceMs = kVgmEndSilenceStepsMs[idx];
      changed = true;
      break;
    }
    case VgmOptionId::HardStopOld:
      gAudio.vgmOptions.hardStopOld = !gAudio.vgmOptions.hardStopOld;
      changed = true;
      break;
    case VgmOptionId::IgnoreVolGain:
      gAudio.vgmOptions.ignoreVolGain = !gAudio.vgmOptions.ignoreVolGain;
      changed = true;
      break;
    case VgmOptionId::MasterVolume: {
      int next = gAudio.vgmOptions.masterVolumeStep +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = kVgmVolumeStepCount - 1;
      if (next >= kVgmVolumeStepCount) next = 0;
      gAudio.vgmOptions.masterVolumeStep = next;
      changed = true;
      break;
    }
    case VgmOptionId::PhaseInvert: {
      int next = static_cast<int>(gAudio.vgmOptions.phaseInvert) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 3;
      if (next > 3) next = 0;
      gAudio.vgmOptions.phaseInvert = static_cast<VgmPhaseInvert>(next);
      changed = true;
      break;
    }
    default:
      break;
  }
  if (changed && isAudioMode(AudioMode::Vgm)) {
    gAudio.state.vgm.applyOptions(gAudio.vgmOptions);
    uint64_t totalFrames = 0;
    if (gAudio.state.vgm.getTotalFrames(&totalFrames)) {
      gAudio.state.totalFrames.store(totalFrames);
    } else {
      gAudio.state.totalFrames.store(0);
    }
  }
  return changed;
}

bool audioAdjustVgmDeviceOption(uint32_t deviceId, VgmDeviceOptionId id,
                                int direction) {
  if (direction == 0) return false;

  VgmDeviceOptions options{};
  if (!audioGetVgmDeviceOptions(deviceId, &options)) {
    return false;
  }

  bool changed = false;
  switch (id) {
    case VgmDeviceOptionId::Mute:
      options.muted = !options.muted;
      changed = true;
      break;
    case VgmDeviceOptionId::Core: {
      const VgmDeviceInfo* info = findVgmDeviceInfo(deviceId);
      if (!info) break;
      std::vector<uint32_t> cores;
      cores.reserve(info->coreIds.size() + 1);
      cores.push_back(0);
      for (uint32_t coreId : info->coreIds) {
        if (coreId != 0) cores.push_back(coreId);
      }
      int idx = 0;
      for (size_t i = 0; i < cores.size(); ++i) {
        if (cores[i] == options.coreId) {
          idx = static_cast<int>(i);
          break;
        }
      }
      idx = advanceIndex(idx, static_cast<int>(cores.size()), direction);
      uint32_t nextCore = cores[idx];
      if (nextCore != options.coreId) {
        options.coreId = nextCore;
        changed = true;
      }
      break;
    }
    case VgmDeviceOptionId::Resampler: {
      int next = static_cast<int>(options.resamplerMode) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      options.resamplerMode = static_cast<uint8_t>(next);
      changed = true;
      break;
    }
    case VgmDeviceOptionId::SampleRateMode: {
      int next = static_cast<int>(options.sampleRateMode) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      options.sampleRateMode = static_cast<uint8_t>(next);
      changed = true;
      break;
    }
    case VgmDeviceOptionId::SampleRate: {
      int idx = findIndex(
          kVgmSampleRateSteps,
          sizeof(kVgmSampleRateSteps) / sizeof(kVgmSampleRateSteps[0]),
          options.sampleRate);
      idx = advanceIndex(
          idx,
          static_cast<int>(sizeof(kVgmSampleRateSteps) /
                           sizeof(kVgmSampleRateSteps[0])),
          direction);
      options.sampleRate = kVgmSampleRateSteps[idx];
      changed = true;
      break;
    }
    default:
      break;
  }

  if (!changed) return false;

  gAudio.vgmDeviceOverrides[deviceId] = options;
  if (isAudioMode(AudioMode::Vgm)) {
    if (id == VgmDeviceOptionId::Mute) {
      gAudio.state.vgm.setDeviceOptions(deviceId, options);
    } else {
      reloadVgmWithOptions();
    }
  }
  return true;
}
