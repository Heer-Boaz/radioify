#include "video_enhancement.h"

namespace playback_video_enhancement {

VideoEnhancementResult VideoEnhancementPipeline::process(
    const VideoEnhancementRequest& request) {
  (void)request.consumer;
  (void)request.device;
  (void)request.context;

  VideoEnhancementResult result;
  result.backend = VideoEnhancementBackend::None;
  if (!request.input) {
    return result;
  }

  result.frame = request.input;
  result.frameAvailable = true;
  result.frameChanged = request.frameChanged || request.forceRefresh;
  return result;
}

const char* VideoEnhancementPipeline::backendName() const {
  return "none";
}

}  // namespace playback_video_enhancement
