#include "video_frame_cache_update.h"

#include <cstddef>

namespace playback_video_frame_cache {

bool update(GpuVideoFrameCache& cache, ID3D11Device* device,
            ID3D11DeviceContext* context, const VideoFrame& frame) {
  if (!device || !context) {
    return false;
  }

  if (frame.format == VideoPixelFormat::HWTexture) {
    if (!frame.hwTexture) {
      return false;
    }
    D3D11_TEXTURE2D_DESC desc{};
    frame.hwTexture->GetDesc(&desc);
    const bool is10Bit = desc.Format == DXGI_FORMAT_P010 ||
                         desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM;
    return cache.Update(device, context, frame.hwTexture.Get(),
                        frame.hwTextureArrayIndex, frame.width, frame.height,
                        frame.fullRange, frame.yuvMatrix, frame.yuvTransfer,
                        is10Bit ? 10 : 8, frame.rotationQuarterTurns);
  }

  if (frame.format == VideoPixelFormat::NV12 ||
      frame.format == VideoPixelFormat::P010) {
    if (frame.stride <= 0 || frame.planeHeight <= 0 || frame.yuv.empty()) {
      return false;
    }
    const size_t strideBytes = static_cast<size_t>(frame.stride);
    const size_t planeHeight = static_cast<size_t>(frame.planeHeight);
    const size_t yBytes = strideBytes * planeHeight;
    if (yBytes / strideBytes != planeHeight) {
      return false;
    }
    return cache.UpdateNV12(
        device, context, frame.yuv.data(), frame.stride, frame.planeHeight,
        frame.width, frame.height, frame.fullRange, frame.yuvMatrix,
        frame.yuvTransfer, frame.format == VideoPixelFormat::P010 ? 10 : 8,
        frame.rotationQuarterTurns);
  }

  if (frame.format == VideoPixelFormat::RGB32 ||
      frame.format == VideoPixelFormat::ARGB32) {
    if (frame.rgba.empty()) {
      return false;
    }
    const int stride = frame.stride > 0 ? frame.stride : frame.width * 4;
    return cache.Update(device, context, frame.rgba.data(), stride, frame.width,
                        frame.height, frame.rotationQuarterTurns);
  }

  return false;
}

}  // namespace playback_video_frame_cache
