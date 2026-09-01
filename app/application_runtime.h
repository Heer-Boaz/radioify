#pragma once

#include <memory>

#include "app/app_common.h"
#include "app/media_processing_actions.h"
#include "app/media_processing_coordinator.h"
#include "app/playback_queue.h"
#include "audio/audioplayback.h"
#include "playback/video/gpu/gpu_runtime.h"

namespace playback_session {
class SubtitleLoadService;
}
namespace playback_video_chapters {
class Service;
}

// Process-wide application services. The UI borrows these services; it does
// not decide their lifetime or shutdown order.
class ApplicationRuntime {
 public:
  explicit ApplicationRuntime(const Options& options);
  ~ApplicationRuntime();

  ApplicationRuntime(const ApplicationRuntime&) = delete;
  ApplicationRuntime& operator=(const ApplicationRuntime&) = delete;

  AudioPlaybackRuntime& audioPlayback() { return audioPlayback_; }
  GpuRuntime& gpu() { return gpu_; }
  playback_queue::Queue& playbackQueue() { return playbackQueue_; }
  media_processing::Coordinator& mediaProcessing() { return mediaProcessing_; }
  media_processing::Actions& mediaActions() { return mediaActions_; }
  playback_session::SubtitleLoadService& subtitleLoader() {
    return *subtitleLoader_;
  }
  playback_video_chapters::Service& chapterAnalysis() {
    return *chapterAnalysis_;
  }

 private:
  // GPU and audio must outlive every surface/session and media worker that
  // borrows them.
  GpuRuntime gpu_;
  AudioPlaybackRuntime audioPlayback_;
  playback_queue::Queue playbackQueue_;
  media_processing::Coordinator mediaProcessing_;
  media_processing::Actions mediaActions_;
  std::unique_ptr<playback_session::SubtitleLoadService> subtitleLoader_;
  std::unique_ptr<playback_video_chapters::Service> chapterAnalysis_;
};

AudioPlaybackConfig audioPlaybackConfigFor(const Options& options);
