#pragma once

#include "videodecoder.h"

#include <memory>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;

namespace playback_video_enhancement {

enum class VideoEnhancementConsumer {
  FramebufferVideo,
  TextGridInput,
};

enum class VideoEnhancementBackend {
  None,
  NvidiaD3D11VideoProcessor,
};

struct VideoEnhancementRequest {
  const VideoFrame* input = nullptr;
  VideoEnhancementConsumer consumer = VideoEnhancementConsumer::FramebufferVideo;
  ID3D11Device* device = nullptr;
  ID3D11DeviceContext* context = nullptr;
  int targetWidth = 0;
  int targetHeight = 0;
  bool frameChanged = false;
  bool forceRefresh = false;
  bool targetHdrOutput = false;
};

struct VideoEnhancementResult {
  const VideoFrame* frame = nullptr;
  bool frameAvailable = false;
  bool frameChanged = false;
  bool enhanced = false;
  VideoEnhancementBackend backend = VideoEnhancementBackend::None;
  std::string debugLine;
};

class VideoEnhancementBackendState;

class VideoEnhancementPipeline {
 public:
  VideoEnhancementPipeline();
  ~VideoEnhancementPipeline();

  VideoEnhancementResult process(const VideoEnhancementRequest& request);
  const char* backendName() const;

 private:
  std::unique_ptr<VideoEnhancementBackendState> backend_;
};

}  // namespace playback_video_enhancement
