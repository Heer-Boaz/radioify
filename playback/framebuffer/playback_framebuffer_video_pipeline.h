#pragma once

#include "gpu/videoprocessor.h"
#include "videodecoder.h"
#include "video_enhancement.h"

namespace playback_framebuffer_video_pipeline {

struct FrameRequest {
  const VideoFrame* frame = nullptr;
  GpuVideoFrameCache* frameCache = nullptr;
  bool frameChanged = false;
  bool forceRefresh = false;
  bool textGridPresentationActive = false;
};

struct FrameResult {
  const VideoFrame* frame = nullptr;
  bool frameAvailable = false;
  bool framebufferFrameChanged = false;
  bool textGridFrameChanged = false;
};

class Pipeline {
 public:
  FrameResult process(const FrameRequest& request);

 private:
  playback_video_enhancement::VideoEnhancementPipeline enhancement_;
};

}  // namespace playback_framebuffer_video_pipeline
