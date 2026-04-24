#pragma once

#include "pipeline.h"

#include <memory>
#include <string>

namespace playback_video_enhancement {

class VideoEnhancementBackendState {
 public:
  virtual ~VideoEnhancementBackendState() = default;

  virtual const char* name() const = 0;
  virtual VideoEnhancementBackend backend() const = 0;
  virtual bool process(const VideoEnhancementRequest& request,
                       VideoEnhancementResult& result) = 0;
};

std::unique_ptr<VideoEnhancementBackendState>
createNvidiaD3D11VideoProcessorBackend();

std::string buildVideoEnhancementDebugLine(
    const VideoEnhancementRequest& request, const char* backendName,
    bool active, const char* reason);

}  // namespace playback_video_enhancement
