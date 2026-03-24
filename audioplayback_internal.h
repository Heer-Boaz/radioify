#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "clock.h"
#include "ffmpegaudio.h"
#include "flacaudio.h"
#include "gmeaudio.h"
#include "gsfaudio.h"
#include "kssaudio.h"
#include "kssoptions.h"
#include "midiaudio.h"
#include "m4adecoder.h"
#include "miniaudio.h"
#include "nsfoptions.h"
#include "psfaudio.h"
#include "radio.h"
#include "radiopreview.h"
#include "vgmaudio.h"
#include "vgmoptions.h"

constexpr uint32_t kMsxClockHz = 3579545u;
constexpr float kAuditionGain = 0.28f;
constexpr int kVgmSpeedStepCount = 6;
constexpr int kVgmVolumeStepCount = 6;
constexpr int kVgmLoopSteps[] = {0, 1, 2, 3, 4, 6, 8, 12, 16};
constexpr int kVgmFadeStepsMs[] = {0, 1000, 2000, 3000, 5000, 8000, 10000, 15000};
constexpr int kVgmEndSilenceStepsMs[] = {0, 250, 500, 1000, 2000, 3000, 5000};
constexpr uint32_t kVgmSampleRateSteps[] = {
    0, 11025, 22050, 32000, 44100, 48000, 88200, 96000};

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
                                uint64_t startFrame, int trackIndex,
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
    std::memcpy(buf.data() + wi * channels, in, first * channels * sizeof(float));
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

struct AudioMetadata {
  uint64_t wpos;
  int64_t ptsUs;
  int serial;
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

extern AudioPlaybackState gAudio;

template <typename T>
int findIndex(const T* values, size_t count, T value) {
  for (size_t i = 0; i < count; ++i) {
    if (values[i] == value) return static_cast<int>(i);
  }
  return 0;
}

inline int advanceIndex(int idx, int count, int direction) {
  if (count <= 0) return 0;
  if (direction > 0) {
    ++idx;
    if (idx >= count) idx = 0;
  } else {
    --idx;
    if (idx < 0) idx = count - 1;
  }
  return idx;
}

AudioMode currentAudioMode();
bool isAudioMode(AudioMode mode);
void startAuditionWorker(AuditionTone tone);
void stopAuditionWorker();
void audioStreamReset(uint64_t framePos);
bool loadFileAt(const std::filesystem::path& file, uint64_t startFrame,
               int trackIndex);
float renderAuditionSample(AuditionTone& tone);
const VgmDeviceInfo* findVgmDeviceInfo(uint32_t deviceId);
