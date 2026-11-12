#pragma once

#include <stdexcept>

#include "miniaudio.h"

// A single decoder worker consumes this retained wake-up. On Windows,
// miniaudio implements it as an auto-reset event, so the audio callback can
// signal it without acquiring the queue mutex or losing an early signal.
class AudioDecoderWake {
 public:
  AudioDecoderWake() {
    if (ma_event_init(&event_) != MA_SUCCESS) {
      throw std::runtime_error("Failed to create audio decoder wake event.");
    }
  }

  ~AudioDecoderWake() { ma_event_uninit(&event_); }

  AudioDecoderWake(const AudioDecoderWake&) = delete;
  AudioDecoderWake& operator=(const AudioDecoderWake&) = delete;

  void signal() noexcept { (void)ma_event_signal(&event_); }
  void wait() noexcept { (void)ma_event_wait(&event_); }

 private:
  ma_event event_{};
};
