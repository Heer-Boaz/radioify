#include "output_clock_source.h"

#include <algorithm>

#include "audioplayback.h"

namespace playback_audio_output_clock_source {
namespace {

constexpr int64_t kMinAudioClockFreshnessUs = 1000000;
constexpr int64_t kMaxAudioClockFreshnessUs = 5000000;

int64_t computeAudioClockFreshnessUs(const AudioPerfStats& stats) {
  if (stats.sampleRate == 0) {
    return kMinAudioClockFreshnessUs;
  }

  uint64_t deviceBufferFrames = stats.bufferFrames;
  if (deviceBufferFrames == 0 && stats.periodFrames > 0) {
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

playback_video_main_clock::AudioClockStatus sample(bool audioActive, int64_t nowUs,
                                             bool audioMayDriveMaster) {
  playback_video_main_clock::AudioClockStatus status;
  status.active = audioActive;
  status.mayDriveMaster = audioMayDriveMaster;

  AudioPerfStats stats = audioGetPerfStats();
  status.lastUpdatedUs = audioStreamClockLastUpdatedUs();
  status.ready = audioStreamClockReady();
  status.fresh = status.lastUpdatedUs > 0 &&
                 (nowUs - status.lastUpdatedUs) <=
                     computeAudioClockFreshnessUs(stats);
  status.starved = audioStreamStarved();
  status.bufferedFrames = audioStreamBufferedFrames();
  status.deviceBufferFrames = stats.bufferFrames;
  if (status.deviceBufferFrames == 0 && stats.periodFrames > 0) {
    status.deviceBufferFrames = static_cast<size_t>(stats.periodFrames) * 2ULL;
  }
  status.sampleRate = stats.sampleRate;
  status.serial = audioStreamSerial();
  status.us = audioStreamClockUs(nowUs);
  return status;
}

void prime(int serial, int64_t targetPtsUs) {
  audioStreamPrimeClock(serial, targetPtsUs);
}

uint64_t updateCounter() { return audioStreamUpdateCounter(); }

uint64_t waitForUpdate(uint64_t lastCounter, int timeoutMs) {
  return audioStreamWaitForUpdate(lastCounter, timeoutMs);
}

}  // namespace playback_audio_output_clock_source
