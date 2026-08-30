#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "audio/analysis/melody_types.h"
#include "audio/playback_session.h"
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

struct AudioPlaybackState;

// Owns an audio playback session. Keep this object alive for every dependent
// player and worker so they are joined before the session is shut down.
class AudioPlaybackRuntime final : public audio_playback::Session {
 public:
  explicit AudioPlaybackRuntime(const AudioPlaybackConfig& config);
  ~AudioPlaybackRuntime();

  AudioPlaybackRuntime(const AudioPlaybackRuntime&) = delete;
  AudioPlaybackRuntime& operator=(const AudioPlaybackRuntime&) = delete;
  AudioPlaybackRuntime(AudioPlaybackRuntime&&) = delete;
  AudioPlaybackRuntime& operator=(AudioPlaybackRuntime&&) = delete;

  // Application-facing audio-player API. Decoder and stream details stay
  // internal; shells receive this owner instead of reaching into shared state.
  bool enabled() const;
  bool ready() const;
  audio_playback::FileStartResult startFile(
      const std::filesystem::path& file, int trackIndex = 0) override;
  void stop() override;
  AudioPlaybackSnapshot snapshot() const override;
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
  void play() override;
  void pause() override;
  void togglePause() override;
  void seekBy(int direction);
  void seekToRatio(double ratio) override;
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
      const std::function<bool()>& cancellationRequested,
      std::string* error,
      const std::function<bool()>& outputCommitStarted = {}) const;
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

 private:
  std::unique_ptr<AudioPlaybackState> state_;
};

bool audioScanKssInstruments(const std::filesystem::path& file, int trackIndex,
                             uint32_t sampleRate,
                             KssPlaybackOptions options,
                             const std::function<bool()>& cancellationRequested,
                             std::vector<KssInstrumentProfile>* out,
                             std::string* error);
bool audioScanVgmMetadata(const std::filesystem::path& file,
                          std::vector<VgmMetadataEntry>* out,
                          std::string* error);
bool audioScanVgmDevices(const std::filesystem::path& file, uint32_t channels,
                         uint32_t sampleRate, VgmDeviceCatalog* out,
                         std::string* error);
