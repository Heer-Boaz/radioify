#include "audio_output_timeline.h"

#include <algorithm>

#include "audioplayback.h"

namespace playback_audio_output_timeline {

void Controller::reset() { clockReacquire_.reset(); }

ResetResult Controller::resetForSerial(int serial, int64_t targetUs,
                                       bool reacquireClock) {
  ResetResult result;
  if (serial <= 0) {
    return result;
  }

  result.applied = true;
  result.serial = serial;
  result.targetUs = (std::max)(int64_t{0}, targetUs);
  audioStreamFlushSerial(serial);
  if (result.targetUs > 0) {
    audioStreamDiscardUntil(result.targetUs);
  }

  if (reacquireClock) {
    clockReacquire_.require(serial, result.targetUs);
    result.reacquireStarted = true;
  }
  return result;
}

void Controller::cancelClockReacquire(int serial) {
  clockReacquire_.cancel(serial);
}

playback_audio_clock_reacquire::Snapshot Controller::clockGateSnapshot(
    int currentSerial) const {
  return clockReacquire_.snapshot(currentSerial);
}

void Controller::noteQueuedAudio(int serial, int64_t ptsUs,
                                 uint64_t writtenFrames) {
  clockReacquire_.noteQueuedAudio(serial, ptsUs, writtenFrames);
}

}  // namespace playback_audio_output_timeline
