#include "playback_clock.h"

#include <atomic>

#include "audioplayback.h"

namespace playback_clock {
namespace {

constexpr int64_t kAudioClockFreshnessUs = 1000000;

}  // namespace

Snapshot sample(bool audioActive, int currentSerial, const Clock& videoClock,
                int64_t nowUs) {
  Snapshot snapshot;
  snapshot.audioClockUpdatedUs = audioStreamClockLastUpdatedUs();
  snapshot.audioClockReady = audioStreamClockReady();
  snapshot.audioClockFresh =
      snapshot.audioClockUpdatedUs > 0 &&
      (nowUs - snapshot.audioClockUpdatedUs) <= kAudioClockFreshnessUs;
  snapshot.audioStarved = audioStreamStarved();
  snapshot.audioBufferedFrames = audioStreamBufferedFrames();
  AudioPerfStats stats = audioGetPerfStats();
  snapshot.audioSampleRate = stats.sampleRate;

  if (audioActive && audioStreamSerial() == currentSerial) {
    snapshot.us = audioStreamClockUs(nowUs);
    snapshot.source = PlayerClockSource::Audio;
    return snapshot;
  }

  if (videoClock.is_valid() &&
      videoClock.serial.load(std::memory_order_relaxed) == currentSerial) {
    snapshot.us = videoClock.get(nowUs);
    snapshot.source = PlayerClockSource::Video;
  }
  return snapshot;
}

int64_t resolveCurrentPlaybackUs(const Snapshot& snapshot,
                                 const Clock& videoClock, int currentSerial,
                                 int64_t lastPresentedPtsUs, int64_t nowUs) {
  if (snapshot.source == PlayerClockSource::Audio && snapshot.audioClockReady &&
      !snapshot.audioStarved && snapshot.audioClockFresh && snapshot.us > 0) {
    return snapshot.us;
  }

  if (videoClock.is_valid() &&
      videoClock.serial.load(std::memory_order_relaxed) == currentSerial) {
    return videoClock.get(nowUs);
  }

  return lastPresentedPtsUs > 0 ? lastPresentedPtsUs : 0;
}

void synchronizePrimingClocks(int serial, int64_t ptsUs, Clock& videoClock,
                              int64_t nowUs) {
  audioStreamPrimeClock(serial, ptsUs);
  videoClock.set(ptsUs, nowUs, serial);
}

void synchronizeAudioClockOnly(int serial, int64_t ptsUs) {
  audioStreamSyncClockOnly(serial, ptsUs);
}

}  // namespace playback_clock
