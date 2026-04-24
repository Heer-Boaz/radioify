#include "pipeline.h"

#include "backend.h"

#include <cstdio>

namespace playback_video_enhancement {
namespace {

const char* consumerLabel(VideoEnhancementConsumer consumer) {
  switch (consumer) {
    case VideoEnhancementConsumer::TextGridInput:
      return "textgrid";
    case VideoEnhancementConsumer::FramebufferVideo:
    default:
      return "framebuffer";
  }
}

const char* transferLabel(YuvTransfer transfer) {
  switch (transfer) {
    case YuvTransfer::Pq:
      return "PQ";
    case YuvTransfer::Hlg:
      return "HLG";
    case YuvTransfer::Sdr:
    default:
      return "SDR";
  }
}

const char* hdrIntentLabel(const VideoEnhancementRequest& request) {
  if (!request.targetHdrOutput) {
    return "off";
  }
  if (!request.input) {
    return "no_frame";
  }
  if (request.input->yuvTransfer != YuvTransfer::Sdr) {
    return "source_hdr";
  }
#if defined(RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_SDK) && \
    RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO_SDK
  return "sdr_to_truehdr";
#else
  return "sdr_to_hdr_output";
#endif
}

VideoEnhancementResult passthroughResult(
    const VideoEnhancementRequest& request,
    const std::string& backendDebugLine = {}) {
  VideoEnhancementResult result;
  result.backend = VideoEnhancementBackend::None;
  if (!request.input) {
    result.debugLine =
        buildVideoEnhancementDebugLine(request, "none", false, "no_input");
    return result;
  }

  result.frame = request.input;
  result.frameAvailable = true;
  result.frameChanged = request.frameChanged || request.forceRefresh;
  if (!backendDebugLine.empty()) {
    result.debugLine = backendDebugLine;
  } else if (result.frameChanged) {
    result.debugLine =
        buildVideoEnhancementDebugLine(request, "none", false, "passthrough");
  }
  return result;
}

std::unique_ptr<VideoEnhancementBackendState> createBackend() {
#if defined(_WIN32) && defined(RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO) && \
    RADIOIFY_ENABLE_NVIDIA_RTX_VIDEO
  return createNvidiaD3D11VideoProcessorBackend();
#else
  return nullptr;
#endif
}

}  // namespace

std::string buildVideoEnhancementDebugLine(
    const VideoEnhancementRequest& request, const char* backendName,
    bool active, const char* reason) {
  char line[256];
  const VideoFrame* frame = request.input;
  std::snprintf(
      line, sizeof(line),
      "DBG enh backend=%s active=%d consumer=%s hdr=%s input_trc=%s reason=%s",
      backendName ? backendName : "none", active ? 1 : 0,
      consumerLabel(request.consumer), hdrIntentLabel(request),
      frame ? transferLabel(frame->yuvTransfer) : "none",
      reason ? reason : "unknown");
  return line;
}

VideoEnhancementPipeline::VideoEnhancementPipeline()
    : backend_(createBackend()) {}

VideoEnhancementPipeline::~VideoEnhancementPipeline() = default;

VideoEnhancementResult VideoEnhancementPipeline::process(
    const VideoEnhancementRequest& request) {
  if (backend_) {
    VideoEnhancementResult result;
    if (backend_->process(request, result)) {
      if (result.debugLine.empty()) {
        result.debugLine =
            buildVideoEnhancementDebugLine(request, backend_->name(), true,
                                           "enhanced");
      }
      return result;
    }
    return passthroughResult(request, result.debugLine);
  }
  return passthroughResult(request);
}

const char* VideoEnhancementPipeline::backendName() const {
  if (backend_) {
    return backend_->name();
  }
  return "none";
}

}  // namespace playback_video_enhancement
