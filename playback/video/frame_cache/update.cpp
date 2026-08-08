#include "update.h"

#include <cstddef>
#include <limits>

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
    if (frame.width <= 0 || frame.height <= 0 || (frame.width & 1) != 0 ||
        (frame.height & 1) != 0 || frame.stride <= 0 ||
        frame.planeHeight < frame.height || frame.yuv.empty()) {
      return false;
    }
    const size_t strideBytes = static_cast<size_t>(frame.stride);
    const size_t planeHeight = static_cast<size_t>(frame.planeHeight);
    const size_t bytesPerSample =
        frame.format == VideoPixelFormat::P010 ? size_t{2} : size_t{1};
    const size_t width = static_cast<size_t>(frame.width);
    if (width > (std::numeric_limits<size_t>::max)() / bytesPerSample ||
        strideBytes < width * bytesPerSample) {
      return false;
    }
    const size_t yBytes = strideBytes * planeHeight;
    if (yBytes / strideBytes != planeHeight) {
      return false;
    }
    const size_t uvRows = static_cast<size_t>(frame.height) / 2u;
    if (uvRows > ((std::numeric_limits<size_t>::max)() - yBytes) /
                     strideBytes ||
        frame.yuv.size() < yBytes + uvRows * strideBytes) {
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
    if (frame.width <= 0 || frame.height <= 0 || frame.rgba.empty() ||
        static_cast<size_t>(frame.width) >
            (std::numeric_limits<size_t>::max)() / 4u) {
      return false;
    }
    const size_t rowBytes = static_cast<size_t>(frame.width) * 4u;
    if (rowBytes > static_cast<size_t>((std::numeric_limits<int>::max)())) {
      return false;
    }
    const int stride =
        frame.stride > 0 ? frame.stride : static_cast<int>(rowBytes);
    if (static_cast<size_t>(stride) < rowBytes ||
        static_cast<size_t>(frame.height) >
            (std::numeric_limits<size_t>::max)() /
                static_cast<size_t>(stride) ||
        frame.rgba.size() < static_cast<size_t>(stride) *
                                static_cast<size_t>(frame.height)) {
      return false;
    }
    return cache.Update(device, context, frame.rgba.data(), stride, frame.width,
                        frame.height, frame.rotationQuarterTurns);
  }

  return false;
}

}  // namespace playback_video_frame_cache
