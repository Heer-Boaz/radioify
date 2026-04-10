#include "playback_clock.h"

#include <algorithm>
#include <atomic>

#include "audioplayback.h"

namespace playback_clock {
namespace {

constexpr int64_t kMinAudioClockFreshnessUs = 1000000;
constexpr int64_t kMaxAudioClockFreshnessUs = 5000000;

int64_t computeAudioClockFreshnessUs(const AudioPerfStats& stats) {
  if (stats.sampleRate == 0) {
    return kMinAudioClockFreshnessUs;
  }

  uint64_t deviceBufferFrames = stats.bufferFrames;
  if (deviceBufferFrames == 0 && stats.periodFrames > 0) {
    // Fall back to a double-period window when the backend does not expose the
    // full device buffer size. This mirrors how mature players budget for a
    // couple of hardware periods instead of assuming low-latency output.
    deviceBufferFrames = static_cast<uint64_t>(stats.periodFrames) * 2ULL;
  }
  if (deviceBufferFrames == 0) {
    return kMinAudioClockFreshnessUs;
  }

  int64_t deviceBufferUs = static_cast<int64_t>(
      (deviceBufferFrames * 1000000ULL) / stats.sampleRate);
  if (deviceBufferUs <= 0) {
    return kMinAudioClockFreshnessUs;
  }

  return std::clamp(deviceBufferUs * 4, kMinAudioClockFreshnessUs,
                    kMaxAudioClockFreshnessUs);
}

}  // namespace

Snapshot sample(bool audioActive, int currentSerial, const Clock& videoClock,
                int64_t nowUs) {
  Snapshot snapshot;
  AudioPerfStats stats = audioGetPerfStats();
  snapshot.audioClockUpdatedUs = audioStreamClockLastUpdatedUs();
  snapshot.audioClockReady = audioStreamClockReady();
  int64_t freshnessUs = computeAudioClockFreshnessUs(stats);
  snapshot.audioClockFresh =
      snapshot.audioClockUpdatedUs > 0 &&
      (nowUs - snapshot.audioClockUpdatedUs) <= freshnessUs;
  snapshot.audioStarved = audioStreamStarved();
  snapshot.audioBufferedFrames = audioStreamBufferedFrames();
  snapshot.audioDeviceBufferFrames = stats.bufferFrames;
  if (snapshot.audioDeviceBufferFrames == 0 && stats.periodFrames > 0) {
    snapshot.audioDeviceBufferFrames =
        static_cast<size_t>(stats.periodFrames) * 2ULL;
  }
  snapshot.audioSampleRate = stats.sampleRate;

  if (audioActive && audioStreamSerial() == currentSerial) {
    int64_t audioUs = audioStreamClockUs(nowUs);
    // Guard the video clock against dead or stale audio output. If the device
    // has stopped updating, fall back to the video clock instead of continuing
    // to treat audio as a valid master clock.
    if (snapshot.audioClockReady && snapshot.audioClockFresh &&
        !snapshot.audioStarved && audioUs > 0) {
      snapshot.us = audioUs;
      snapshot.source = PlayerClockSource::Audio;
      return snapshot;
    }
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
