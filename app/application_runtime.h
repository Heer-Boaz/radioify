#pragma once

#include "app/app_common.h"
#include "app/media_processing_coordinator.h"
#include "app/playback_queue.h"
#include "audio/audioplayback.h"

// Process-wide application services. The UI borrows these services; it does
// not decide their lifetime or shutdown order.
class ApplicationRuntime {
 public:
  explicit ApplicationRuntime(const Options& options);
  ~ApplicationRuntime();

  ApplicationRuntime(const ApplicationRuntime&) = delete;
  ApplicationRuntime& operator=(const ApplicationRuntime&) = delete;

  playback_queue::Queue& playbackQueue() { return playbackQueue_; }
  media_processing::Coordinator& mediaProcessing() { return mediaProcessing_; }

 private:
  // Audio must outlive media-processing workers that can call audio analysis.
  AudioPlaybackRuntime audioPlayback_;
  playback_queue::Queue playbackQueue_;
  media_processing::Coordinator mediaProcessing_;
};

AudioPlaybackConfig audioPlaybackConfigFor(const Options& options);
