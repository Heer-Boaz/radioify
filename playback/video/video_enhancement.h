#pragma once

#include "videodecoder.h"

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace playback_video_enhancement {

enum class VideoEnhancementConsumer {
  FramebufferVideo,
  TextGridInput,
};

enum class VideoEnhancementBackend {
  None,
};

struct VideoEnhancementRequest {
  const VideoFrame* input = nullptr;
  VideoEnhancementConsumer consumer = VideoEnhancementConsumer::FramebufferVideo;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  bool frameChanged = false;
  bool forceRefresh = false;
};

struct VideoEnhancementResult {
  const VideoFrame* frame = nullptr;
  bool frameAvailable = false;
  bool frameChanged = false;
  bool enhanced = false;
  VideoEnhancementBackend backend = VideoEnhancementBackend::None;
};

class VideoEnhancementPipeline {
 public:
  VideoEnhancementResult process(const VideoEnhancementRequest& request);
  const char* backendName() const;
};

}  // namespace playback_video_enhancement
