#pragma once

#include "gpu/videoprocessor.h"
#include "videodecoder.h"
#include "video_enhancement.h"

#include <string>

namespace playback_framebuffer_video_pipeline {

struct FrameRequest {
  const VideoFrame* frame = nullptr;
  GpuVideoFrameCache* frameCache = nullptr;
  int targetWidth = 0;
  int targetHeight = 0;
  bool frameChanged = false;
  bool forceRefresh = false;
  bool textGridPresentationActive = false;
  bool targetHdrOutput = false;
};

struct FrameResult {
  const VideoFrame* frame = nullptr;
  bool frameAvailable = false;
  bool framebufferFrameChanged = false;
  bool textGridFrameChanged = false;
  std::string debugLine;
};

class Pipeline {
 public:
  FrameResult process(const FrameRequest& request);

 private:
  playback_video_enhancement::VideoEnhancementPipeline enhancement_;
  int lastTargetWidth_ = 0;
  int lastTargetHeight_ = 0;
  std::string lastDebugLine_;
};

}  // namespace playback_framebuffer_video_pipeline
