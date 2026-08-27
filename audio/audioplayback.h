#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "audio/analysis/melody_types.h"
#include "audio/playback_snapshot.h"
#include "audiofilter/radio1938/radio_reception_profile.h"
#include "core/runtime_defaults.h"
#include "kssoptions.h"
#include "nsfoptions.h"
#include "radio_filter_mode.h"
#include "stream_reset.h"
#include "vgmoptions.h"

struct AudioPerfStats {
  uint64_t callbacks = 0;
  uint64_t framesRequested = 0;
  uint64_t framesRead = 0;
  uint64_t shortReads = 0;
  uint64_t silentFrames = 0;
  uint32_t lastCallbackFrames = 0;
  uint32_t lastFramesRead = 0;
  uint32_t periodFrames = 0;
  uint32_t periods = 0;
  uint32_t bufferFrames = 0;
  uint32_t sampleRate = 0;
  uint32_t channels = 0;
  bool usingFfmpeg = false;
};

struct AudioPlaybackConfig {
  bool enableAudio = kDefaultAudioPlaybackEnabled;
  bool enableRadio = kDefaultRadioFilterEnabled;
  bool mono = kDefaultMonoAudioEnabled;
  bool dry = kDefaultDryAudioEnabled;
  std::string radioSettingsPath;
  std::string radioPresetName;
  RadioReceiverProfile radioReceiverProfile = kDefaultRadioReceiverProfile;
  RadioReceptionProfile radioReceptionProfile =
      kDefaultRadioReceptionProfile;
  int bwHz = kDefaultRadioBandwidthHz;
  double noise = kDefaultRadioNoiseAmount;
};

struct KssInstrumentProfile {
  KssInstrumentDevice device = KssInstrumentDevice::None;
  uint32_t hash = 0;
  std::vector<uint8_t> data;
  uint8_t volume = 0;
};

// Owns the process-wide audio playback state. Keep this object alive for every
// consumer of the audio API; dependent workers must be declared after it so
// they are joined before audio is shut down.
class AudioPlaybackRuntime {
 public:
  explicit AudioPlaybackRuntime(const AudioPlaybackConfig& config);
  ~AudioPlaybackRuntime();

  AudioPlaybackRuntime(const AudioPlaybackRuntime&) = delete;
  AudioPlaybackRuntime& operator=(const AudioPlaybackRuntime&) = delete;
  AudioPlaybackRuntime(AudioPlaybackRuntime&&) = delete;
  AudioPlaybackRuntime& operator=(AudioPlaybackRuntime&&) = delete;

  // Application-facing audio-player API. Low-level decoder and stream
  // functions below remain available to the playback engine, while shells
  // receive this owner instead of reaching into process globals.
  bool enabled() const;
  bool ready() const;
  bool startFile(const std::filesystem::path& file, int trackIndex = 0);
  void stop();
  AudioPlaybackSnapshot snapshot() const;
  AudioPerfStats perfStats() const;
  bool startStream(uint64_t totalFrames);
  void stopStream();
  size_t streamBufferedFrames() const;
  size_t streamCapacityFrames() const;
  int64_t streamOldestPtsUs() const;
  bool writeStreamSamples(const float* interleaved, uint64_t frames,
                          int64_t ptsUs, int serial, bool allowBlock,
                          uint64_t* writtenFrames);
  void primeStreamClock(int serial, int64_t targetPtsUs);
  void flushStreamSerial(int serial, int64_t discardUntilUs);
  void resetStream(uint64_t framePosition);
  AudioStreamReset lastAppliedStreamReset() const;
  void setStreamEnd(bool atEnd);
  int streamSerial() const;
  int64_t streamClockUs(int64_t nowUs) const;
  int64_t streamClockLastUpdatedUs() const;
  bool streamClockReady() const;
  bool streamStarved() const;
  uint64_t streamUpdateCounter() const;
  uint64_t waitForStreamUpdate(uint64_t lastCounter, int timeoutMs) const;
  void setHold(bool hold);
  void play();
  void pause();
  void togglePause();
  void seekBy(int direction);
  void seekToRatio(double ratio);
  void cycleRadioFilter();
  void toggle50Hz();
  void adjustVolume(float delta);
  RadioFilterMode radioFilterMode() const;
  bool radioEnabled() const;
  bool supports50HzToggle() const;
  std::string warning() const;
  AudioMelodyInfo melodyInfo() const;
  AudioMelodyAnalysisState melodyAnalysisState() const;
  bool analyzeFileToMelodyFile(
      const std::filesystem::path& file, int trackIndex,
      const std::filesystem::path& outputFile,
      const std::function<void(float)>& progressCallback,
      std::string* error) const;
  bool canAnalyzeFile(const std::filesystem::path& file) const;
  KssPlaybackOptions kssOptions() const;
  bool kssInstrumentAuditionState(KssInstrumentDevice* device,
                                  uint32_t* hash) const;
  bool startKssInstrumentAudition(const KssInstrumentProfile& profile);
  bool stopKssInstrumentAudition();
  bool adjustKssOption(KssOptionId id, int direction = 1);
  NsfPlaybackOptions nsfOptions() const;
  bool adjustNsfOption(NsfOptionId id, int direction = 1);
  VgmPlaybackOptions vgmOptions() const;
  bool adjustVgmOption(VgmOptionId id, int direction = 1);
  bool vgmDeviceOptions(uint32_t deviceId, VgmDeviceOptions* out) const;
  bool adjustVgmDeviceOption(const VgmDeviceInfo& device,
                             const VgmDeviceOptions& baseline,
                             VgmDeviceOptionId id, int direction = 1);
};

bool audioIsEnabled();
bool audioIsReady();

bool audioStartFile(const std::filesystem::path& file, int trackIndex = 0);
bool audioStartFileAt(const std::filesystem::path& file, double startSec,
                      int trackIndex = 0);
void audioStop();
std::optional<AudioPlaybackSource> audioGetPlaybackSource();
AudioPlaybackSnapshot audioGetPlaybackSnapshot();

double audioGetTimeSec();
double audioGetTotalSec();
bool audioIsSeeking();
double audioGetSeekTargetSec();

bool audioIsPaused();
bool audioIsFinished();
bool audioIsRadioEnabled();
RadioFilterMode audioGetRadioFilterMode();
std::string_view audioGetRadioFilterLabel();
bool audioIsHolding();

AudioPerfStats audioGetPerfStats();

void audioPlay();
void audioPause();
void audioTogglePause();
void audioSeekBy(int direction);
void audioSeekToRatio(double ratio);
void audioSeekToSec(double sec);
void audioCycleRadioFilter();
void audioToggle50Hz();
void audioSetHold(bool hold);
void audioAdjustVolume(float delta);
float audioGetVolume();
float audioGetUnclippedOutputPeak();
AudioMelodyInfo audioGetMelodyInfo();
AudioMelodyAnalysisState audioGetMelodyAnalysisState();
bool audioAnalyzeFileToMelodyFile(const std::filesystem::path& file,
                                  int trackIndex,
                                  const std::filesystem::path& outputFile,
                                  const std::function<void(float)>& progressCallback,
                                  std::string* error);
bool audioCanAnalyzeFileToMelodyFile(const std::filesystem::path& file);
std::string audioGetWarning();
bool audioIs50HzEnabled();
bool audioSupports50HzToggle();
KssPlaybackOptions audioGetKssOptionState();
bool audioAdjustKssOption(KssOptionId id, int direction = 1);
bool audioGetKssInstrumentRegs(KssInstrumentDevice device,
                               std::vector<uint8_t>* out);
bool audioSetKssInstrumentPreview(KssInstrumentDevice device, int channel);
bool audioGetKssInstrumentAuditionState(KssInstrumentDevice* device,
                                        uint32_t* hash);
bool audioStartKssInstrumentAudition(const KssInstrumentProfile& profile);
bool audioStopKssInstrumentAudition();
bool audioScanKssInstruments(const std::filesystem::path& file, int trackIndex,
                             uint32_t sampleRate,
                             KssPlaybackOptions options,
                             const std::function<bool()>& cancellationRequested,
                             std::vector<KssInstrumentProfile>* out,
                             std::string* error);
NsfPlaybackOptions audioGetNsfOptionState();
bool audioAdjustNsfOption(NsfOptionId id, int direction = 1);
VgmPlaybackOptions audioGetVgmOptionState();
bool audioAdjustVgmOption(VgmOptionId id, int direction = 1);
bool audioScanVgmMetadata(const std::filesystem::path& file,
                          std::vector<VgmMetadataEntry>* out,
                          std::string* error);
bool audioScanVgmDevices(const std::filesystem::path& file, uint32_t channels,
                         uint32_t sampleRate, VgmDeviceCatalog* out,
                         std::string* error);
bool audioGetVgmDeviceOptions(uint32_t deviceId, VgmDeviceOptions* out);
bool audioAdjustVgmDeviceOption(const VgmDeviceInfo& device,
                                const VgmDeviceOptions& baseline,
                                VgmDeviceOptionId id,
                                int direction = 1);
