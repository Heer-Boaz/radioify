#include "video_pipeline.h"

#include <mutex>

#include <wrl/client.h>

#include "playback/video/gpu/gpu_shared.h"
#include "playback/video/frame_cache/update.h"

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
  const bool targetChanged =
      request.targetWidth != lastTargetWidth_ ||
      request.targetHeight != lastTargetHeight_;
  lastTargetWidth_ = request.targetWidth;
  lastTargetHeight_ = request.targetHeight;

  playback_video_enhancement::VideoEnhancementRequest enhancementRequest;
  enhancementRequest.input = request.frame;
  enhancementRequest.consumer =
      enhancementConsumer(request.textGridPresentationActive);
  enhancementRequest.targetWidth = request.targetWidth;
  enhancementRequest.targetHeight = request.targetHeight;
  enhancementRequest.frameChanged = request.frameChanged;
  enhancementRequest.forceRefresh = request.forceRefresh || targetChanged;
  enhancementRequest.targetHdrOutput = request.targetHdrOutput;

  ID3D11Device* device = nullptr;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
  if (needsFrameProcessing(request) || (request.frame && targetChanged)) {
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
  if (!enhancementResult.debugLine.empty()) {
    lastDebugLine_ = enhancementResult.debugLine;
  }

  FrameResult result;
  result.frame = enhancementResult.frame;
  result.frameAvailable = enhancementResult.frameAvailable;
  result.framebufferFrameChanged = framebufferFrameChanged;
  result.textGridFrameChanged = enhancementResult.frameChanged;
  result.debugLine = lastDebugLine_;
  return result;
}

}  // namespace playback_framebuffer_video_pipeline
