#include "playback_main_clock.h"

#include <atomic>
#include <cmath>

namespace playback_main_clock {
namespace {

bool audioCanDriveMaster(const SampleRequest& request) {
  const AudioClockStatus& audio = request.audio;
  return audio.active && audio.mayDriveMaster &&
         audio.serial == request.currentSerial && audio.ready && audio.fresh &&
         !audio.starved && audio.us > 0;
}

bool videoCanDriveMaster(const SampleRequest& request) {
  return request.video.ready && request.video.serial == request.currentSerial &&
         request.video.us > 0;
}

TrackClockStatus readVideoClock(const Clock& clock, int currentSerial,
                                int64_t nowUs) {
  TrackClockStatus status;
  status.serial = clock.serial.load(std::memory_order_relaxed);
  if (!clock.is_valid() || status.serial != currentSerial) {
    return status;
  }
  status.ready = true;
  status.us = clock.get(nowUs);
  return status;
}

}  // namespace

void Controller::reset() {
  videoClock_.reset(0);
  lastSource_.store(static_cast<int>(PlayerClockSource::None),
                    std::memory_order_relaxed);
  lastSerial_.store(0, std::memory_order_relaxed);
  paused_.store(true, std::memory_order_relaxed);
}

void Controller::setMasterPolicy(MasterPolicy policy) {
  policy_.store(static_cast<int>(policy), std::memory_order_relaxed);
}

MasterPolicy Controller::masterPolicy() const {
  return static_cast<MasterPolicy>(policy_.load(std::memory_order_relaxed));
}

void Controller::startSession(int initialSerial) {
  reset();
  resetForSerial(initialSerial);
}

void Controller::resetForSerial(int serial) {
  lastSource_.store(static_cast<int>(PlayerClockSource::None),
                    std::memory_order_relaxed);
  lastSerial_.store(serial, std::memory_order_relaxed);
  paused_.store(true, std::memory_order_relaxed);
  videoClock_.reset(serial);
}

void Controller::changePause(bool paused, int64_t nowUs) {
  bool previous = paused_.exchange(paused, std::memory_order_relaxed);
  if (previous == paused && videoClock_.paused.load(std::memory_order_relaxed) ==
                                paused) {
    return;
  }
  videoClock_.set_paused(paused, nowUs);
}

void Controller::updateVideo(int serial, int64_t ptsUs, int64_t systemUs) {
  lastSerial_.store(serial, std::memory_order_relaxed);
  paused_.store(false, std::memory_order_relaxed);
  videoClock_.set(ptsUs, systemUs, serial);
}

TrackClockStatus Controller::videoStatus(int currentSerial,
                                         int64_t nowUs) const {
  return readVideoClock(videoClock_, currentSerial, nowUs);
}

int64_t Controller::currentVideoUs(int currentSerial,
                                   int64_t lastPresentedPtsUs,
                                   int64_t nowUs) const {
  TrackClockStatus status = videoStatus(currentSerial, nowUs);
  if (status.ready && status.us > 0) {
    return status.us;
  }
  return lastPresentedPtsUs > 0 ? lastPresentedPtsUs : 0;
}

Snapshot Controller::sample(const SampleRequest& request) {
  Snapshot snapshot;
  MasterPolicy policy = masterPolicy();
  snapshot.policy = policy;
  snapshot.systemUs = request.nowUs;
  snapshot.audioClockReady = request.audio.ready;
  snapshot.audioClockFresh = request.audio.fresh;
  snapshot.audioMasterBlocked =
      request.audio.active && !request.audio.mayDriveMaster;
  snapshot.audioStarved = request.audio.starved;
  snapshot.audioClockUs = request.audio.us;
  snapshot.audioClockUpdatedUs = request.audio.lastUpdatedUs;
  snapshot.audioBufferedFrames = request.audio.bufferedFrames;
  snapshot.audioDeviceBufferFrames = request.audio.deviceBufferFrames;
  snapshot.audioSampleRate = request.audio.sampleRate;
  snapshot.videoClockReady = request.video.ready;
  snapshot.videoClockUs = request.video.us;

  const bool audioEligible = audioCanDriveMaster(request);
  const bool videoEligible = videoCanDriveMaster(request);

  if (policy != MasterPolicy::Video && audioEligible) {
    snapshot.us = request.audio.us;
    snapshot.source = PlayerClockSource::Audio;
  } else if (policy != MasterPolicy::Audio && videoEligible) {
    snapshot.us = request.video.us;
    snapshot.source = PlayerClockSource::Video;
  } else if (videoEligible) {
    snapshot.us = request.video.us;
    snapshot.source = PlayerClockSource::Video;
  }

  lastSource_.store(static_cast<int>(snapshot.source),
                    std::memory_order_relaxed);
  lastSerial_.store(request.currentSerial, std::memory_order_relaxed);
  return snapshot;
}

int64_t resolveCurrentPlaybackUs(const Snapshot& snapshot,
                                 int64_t lastPresentedPtsUs) {
  if (snapshot.source != PlayerClockSource::None && snapshot.us > 0) {
    return snapshot.us;
  }
  if (snapshot.videoClockReady && snapshot.videoClockUs > 0) {
    return snapshot.videoClockUs;
  }
  return lastPresentedPtsUs > 0 ? lastPresentedPtsUs : 0;
}

int64_t convertToSystemUs(const Snapshot& snapshot, int64_t streamUs,
                          int64_t fallbackSystemUs) {
  if (snapshot.source == PlayerClockSource::None || snapshot.us <= 0 ||
      snapshot.systemUs <= 0 || snapshot.rate <= 0.0) {
    return fallbackSystemUs;
  }
  double deltaUs = static_cast<double>(streamUs - snapshot.us) / snapshot.rate;
  return snapshot.systemUs + static_cast<int64_t>(std::llround(deltaUs));
}

}  // namespace playback_main_clock
