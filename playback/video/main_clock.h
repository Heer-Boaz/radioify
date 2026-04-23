#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "clock.h"
#include "player.h"

namespace playback_main_clock {

enum class MasterPolicy {
  Auto,
  Audio,
  Video,
};

struct AudioClockStatus {
  bool active = false;
  bool mayDriveMaster = true;
  bool ready = false;
  bool fresh = false;
  bool starved = false;
  int serial = 0;
  int64_t us = 0;
  int64_t lastUpdatedUs = 0;
  size_t bufferedFrames = 0;
  size_t deviceBufferFrames = 0;
  uint32_t sampleRate = 0;
};

struct TrackClockStatus {
  bool ready = false;
  int serial = 0;
  int64_t us = 0;
};

struct SampleRequest {
  int currentSerial = 0;
  int64_t nowUs = 0;
  AudioClockStatus audio;
  TrackClockStatus video;
};

struct Snapshot {
  int64_t us = 0;
  int64_t systemUs = 0;
  double rate = 1.0;
  PlayerClockSource source = PlayerClockSource::None;
  MasterPolicy policy = MasterPolicy::Auto;
  bool audioClockReady = false;
  bool audioClockFresh = false;
  bool audioMasterBlocked = false;
  bool audioStarved = false;
  int64_t audioClockUs = 0;
  int64_t audioClockUpdatedUs = 0;
  size_t audioBufferedFrames = 0;
  size_t audioDeviceBufferFrames = 0;
  uint32_t audioSampleRate = 0;
  bool videoClockReady = false;
  int64_t videoClockUs = 0;
};

class Controller {
 public:
  void reset();
  void setMasterPolicy(MasterPolicy policy);
  MasterPolicy masterPolicy() const;
  void startSession(int initialSerial);
  void resetForSerial(int serial);
  void changePause(bool paused, int64_t nowUs);
  void updateVideo(int serial, int64_t ptsUs, int64_t systemUs);
  TrackClockStatus videoStatus(int currentSerial, int64_t nowUs) const;
  int64_t currentVideoUs(int currentSerial, int64_t lastPresentedPtsUs,
                         int64_t nowUs) const;
  Snapshot sample(const SampleRequest& request);

 private:
  Clock videoClock_;
  std::atomic<int> policy_{static_cast<int>(MasterPolicy::Auto)};
  std::atomic<int> lastSource_{static_cast<int>(PlayerClockSource::None)};
  std::atomic<int> lastSerial_{0};
  std::atomic<bool> paused_{true};
};

int64_t resolveCurrentPlaybackUs(const Snapshot& snapshot,
                                 int64_t lastPresentedPtsUs);

int64_t convertToSystemUs(const Snapshot& snapshot, int64_t streamUs,
                          int64_t fallbackSystemUs);

}  // namespace playback_main_clock
