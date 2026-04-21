#include "playback_framebuffer_video_pipeline.h"

#include <mutex>

#include <wrl/client.h>

#include "gpu_shared.h"
#include "video_frame_cache_update.h"

namespace playback_framebuffer_video_pipeline {
namespace {

playback_video_enhancement::VideoEnhancementConsumer enhancementConsumer(
    bool textGridPresentationActive) {
  return textGridPresentationActive
             ? playback_video_enhancement::VideoEnhancementConsumer::
                   TextGridInput
             : playback_video_enhancement::VideoEnhancementConsumer::
                   FramebufferVideo;
}

bool needsFrameProcessing(const FrameRequest& request) {
  return request.frame && (request.frameChanged || request.forceRefresh);
}

}  // namespace

FrameResult Pipeline::process(const FrameRequest& request) {
  playback_video_enhancement::VideoEnhancementRequest enhancementRequest;
  enhancementRequest.input = request.frame;
  enhancementRequest.consumer =
      enhancementConsumer(request.textGridPresentationActive);
  enhancementRequest.frameChanged = request.frameChanged;
  enhancementRequest.forceRefresh = request.forceRefresh;

  ID3D11Device* device = nullptr;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  if (needsFrameProcessing(request)) {
    device = getSharedGpuDevice();
    if (device) {
      device->GetImmediateContext(&context);
    }
  }
  enhancementRequest.device = device;
  enhancementRequest.context = context.Get();

  playback_video_enhancement::VideoEnhancementResult enhancementResult;
  bool framebufferFrameChanged = false;
  if (context) {
    std::lock_guard<std::recursive_mutex> lock(getSharedGpuMutex());
    enhancementResult = enhancement_.process(enhancementRequest);
    if (!request.textGridPresentationActive && enhancementResult.frameChanged &&
        request.frameCache && enhancementResult.frame) {
      framebufferFrameChanged = playback_video_frame_cache::update(
          *request.frameCache, device, context.Get(), *enhancementResult.frame);
    }
  } else {
    enhancementResult = enhancement_.process(enhancementRequest);
  }

  FrameResult result;
  result.frame = enhancementResult.frame;
  result.frameAvailable = enhancementResult.frameAvailable;
  result.framebufferFrameChanged = framebufferFrameChanged;
  result.textGridFrameChanged = enhancementResult.frameChanged;
  return result;
}

}  // namespace playback_framebuffer_video_pipeline
