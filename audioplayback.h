#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "kssoptions.h"
#include "nsfoptions.h"
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
  bool enableAudio = true;
  bool enableRadio = false;
  bool mono = true;
  bool dry = false;
  int bwHz = 5500;
  double noise = 0.012;
};

struct KssInstrumentProfile {
  KssInstrumentDevice device = KssInstrumentDevice::None;
  uint32_t hash = 0;
  std::vector<uint8_t> data;
  uint8_t volume = 0;
};

void audioInit(const AudioPlaybackConfig& config);
void audioShutdown();

bool audioIsEnabled();
bool audioIsReady();

bool audioStartFile(const std::filesystem::path& file, int trackIndex = 0);
bool audioStartFileAt(const std::filesystem::path& file, double startSec,
                      int trackIndex = 0);
void audioStop();
bool audioStartStream(uint64_t totalFrames);
void audioStopStream();
size_t audioStreamBufferedFrames();
size_t audioStreamCapacityFrames();
int64_t audioStreamOldestPtsUs();
bool audioStreamWriteSamples(const float* interleaved,
                             uint64_t frames,
                             int64_t ptsUs,
                             int serial,
                             bool allowBlock,
                             uint64_t* writtenFrames);
uint64_t audioStreamDropFrames(uint64_t frames);
void audioStreamDiscardUntil(int64_t ptsUs);
void audioStreamSynchronize(int serial, int64_t targetPtsUs);
void audioStreamSyncClockOnly(int serial, int64_t targetPtsUs);
void audioStreamSetBase(int serial, int64_t ptsUs);
void audioStreamSetEnd(bool atEnd);
void audioStreamReset(uint64_t framePos);
void audioStreamFlushSerial(int serial);
int audioStreamSerial();
int64_t audioStreamClockUs(int64_t nowUs);
int64_t audioStreamClockLastUpdatedUs();
bool audioStreamStarved();
bool audioStreamClockReady();

// Task 3: Hardware Callback Driving
// Block until the audio hardware callback has progressed or timeout.
// Returns the new update counter value.
uint64_t audioStreamWaitForUpdate(uint64_t lastCounter, int timeoutMs);
uint64_t audioStreamUpdateCounter();

std::filesystem::path audioGetNowPlaying();
int audioGetTrackIndex();

double audioGetTimeSec();
double audioGetTotalSec();
bool audioIsSeeking();
double audioGetSeekTargetSec();

bool audioIsPrimed();
bool audioIsPaused();
bool audioIsFinished();
bool audioIsRadioEnabled();
bool audioIsHolding();

AudioPerfStats audioGetPerfStats();

void audioTogglePause();
void audioSeekBy(int direction);
void audioSeekToRatio(double ratio);
void audioSeekToSec(double sec);
void audioToggleRadio();
void audioToggle50Hz();
void audioSetHold(bool hold);
void audioAdjustVolume(float delta);
float audioGetVolume();
std::string audioGetWarning();
bool audioIs50HzEnabled();
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
                             std::vector<KssInstrumentProfile>* out,
                             std::string* error);
NsfPlaybackOptions audioGetNsfOptionState();
bool audioAdjustNsfOption(NsfOptionId id, int direction = 1);
VgmPlaybackOptions audioGetVgmOptionState();
bool audioAdjustVgmOption(VgmOptionId id, int direction = 1);
bool audioScanVgmMetadata(const std::filesystem::path& file,
                          std::vector<VgmMetadataEntry>* out,
                          std::string* error);
bool audioScanVgmDevices(const std::filesystem::path& file,
                         std::vector<VgmDeviceInfo>* out,
                         std::string* error);
bool audioGetVgmDeviceOptions(uint32_t deviceId, VgmDeviceOptions* out);
bool audioAdjustVgmDeviceOption(uint32_t deviceId, VgmDeviceOptionId id,
                                int direction = 1);
