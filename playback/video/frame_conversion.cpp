#include "playback/video/frame_conversion.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <utility>

#include "playback/video/color_conversion.h"
#include "playback/video/decoder.h"

namespace playback_video_frame_conversion {

bool toRgba(const VideoFrame& frame, playback_video_image::RgbaImage* image) {
  if (!image || frame.width <= 0 || frame.height <= 0 ||
      frame.rotationQuarterTurns != 0 ||
      static_cast<uint64_t>(frame.width) * 4u >
          (std::numeric_limits<uint32_t>::max)()) {
    return false;
  }
  const uint32_t width = static_cast<uint32_t>(frame.width);
  const uint32_t height = static_cast<uint32_t>(frame.height);
  const uint32_t stride = width * 4u;
  size_t outputBytes = 0;
  if (!playback_video_image::requiredBytes(width, height, stride,
                                           &outputBytes)) {
    return false;
  }
  playback_video_image::RgbaImage converted;
  converted.width = width;
  converted.height = height;
  converted.strideBytes = stride;
  try {
    converted.pixels.resize(outputBytes);
  } catch (const std::bad_alloc&) {
    return false;
  }

  if (frame.format == VideoPixelFormat::RGB32 ||
      frame.format == VideoPixelFormat::ARGB32) {
    const size_t sourceStride = frame.stride > 0
                                    ? static_cast<size_t>(frame.stride)
                                    : static_cast<size_t>(stride);
    const size_t rowBytes = static_cast<size_t>(stride);
    if (sourceStride < rowBytes ||
        static_cast<size_t>(height - 1u) >
            ((std::numeric_limits<size_t>::max)() - rowBytes) / sourceStride ||
        frame.rgba.size() <
            static_cast<size_t>(height - 1u) * sourceStride + rowBytes) {
      return false;
    }
    for (uint32_t y = 0; y < height; ++y) {
      const uint8_t* source =
          frame.rgba.data() + static_cast<size_t>(y) * sourceStride;
      uint8_t* destination =
          converted.pixels.data() + static_cast<size_t>(y) * stride;
      std::copy_n(source, rowBytes, destination);
      if (frame.format == VideoPixelFormat::RGB32) {
        for (uint32_t x = 0; x < width; ++x) {
          destination[x * 4u + 3u] = 255;
        }
      }
    }
    *image = std::move(converted);
    return true;
  }

  if (frame.format != VideoPixelFormat::NV12 &&
      frame.format != VideoPixelFormat::P010) {
    return false;
  }
  const bool p010 = frame.format == VideoPixelFormat::P010;
  const size_t bytesPerSample = p010 ? 2u : 1u;
  if ((frame.width & 1) != 0 || (frame.height & 1) != 0 || frame.stride <= 0 ||
      frame.planeHeight < frame.height || (p010 && (frame.stride & 1) != 0) ||
      static_cast<size_t>(frame.stride) <
          static_cast<size_t>(frame.width) * bytesPerSample) {
    return false;
  }
  const size_t sourceStride = static_cast<size_t>(frame.stride);
  const size_t planeHeight = static_cast<size_t>(frame.planeHeight);
  if (planeHeight > (std::numeric_limits<size_t>::max)() / sourceStride) {
    return false;
  }
  const size_t yBytes = planeHeight * sourceStride;
  const size_t uvRows = static_cast<size_t>(frame.height / 2);
  if (uvRows > ((std::numeric_limits<size_t>::max)() - yBytes) / sourceStride ||
      frame.yuv.size() < yBytes + uvRows * sourceStride) {
    return false;
  }

  const uint8_t* yPlane = frame.yuv.data();
  const uint8_t* uvPlane = yPlane + yBytes;
  for (uint32_t y = 0; y < height; ++y) {
    uint8_t* destination =
        converted.pixels.data() + static_cast<size_t>(y) * stride;
    if (p010) {
      const auto* yRow = reinterpret_cast<const uint16_t*>(
          yPlane + static_cast<size_t>(y) * sourceStride);
      const auto* uvRow = reinterpret_cast<const uint16_t*>(
          uvPlane + static_cast<size_t>(y / 2u) * sourceStride);
      for (uint32_t x = 0; x < width; ++x) {
        const size_t uvIndex = static_cast<size_t>(x / 2u) * 2u;
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        playback_video_color::yuvToRgb8(
            static_cast<int>(yRow[x] >> 6),
            static_cast<int>(uvRow[uvIndex] >> 6),
            static_cast<int>(uvRow[uvIndex + 1u] >> 6), frame.fullRange, 10,
            frame.yuvMatrix, frame.yuvTransfer, red, green, blue);
        destination[x * 4u + 0u] = red;
        destination[x * 4u + 1u] = green;
        destination[x * 4u + 2u] = blue;
        destination[x * 4u + 3u] = 255;
      }
    } else {
      const uint8_t* yRow = yPlane + static_cast<size_t>(y) * sourceStride;
      const uint8_t* uvRow =
          uvPlane + static_cast<size_t>(y / 2u) * sourceStride;
      for (uint32_t x = 0; x < width; ++x) {
        const size_t uvIndex = static_cast<size_t>(x / 2u) * 2u;
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        playback_video_color::yuvToRgb8(
            yRow[x], uvRow[uvIndex], uvRow[uvIndex + 1u], frame.fullRange, 8,
            frame.yuvMatrix, frame.yuvTransfer, red, green, blue);
        destination[x * 4u + 0u] = red;
        destination[x * 4u + 1u] = green;
        destination[x * 4u + 2u] = blue;
        destination[x * 4u + 3u] = 255;
      }
    }
  }
  *image = std::move(converted);
  return true;
}

}  // namespace playback_video_frame_conversion
