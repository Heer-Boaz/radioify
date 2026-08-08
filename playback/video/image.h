#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

namespace playback_video_image {

// Canonical CPU image exchanged at UI/image-codec boundaries. Pixels are
// straight-alpha RGBA8 in top-to-bottom row order; decoder-native planes and
// GPU texture layouts intentionally do not cross this boundary.
struct RgbaImage {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t strideBytes = 0;
  std::vector<uint8_t> pixels;
};

struct RgbaImageView {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t strideBytes = 0;
  const uint8_t* pixels = nullptr;
  size_t sizeBytes = 0;
};

inline bool requiredBytes(uint32_t width, uint32_t height,
                          uint32_t strideBytes, size_t* bytes) {
  if (!bytes || width == 0 || height == 0) return false;
  const size_t resolvedWidth = static_cast<size_t>(width);
  if (resolvedWidth > (std::numeric_limits<size_t>::max)() / 4u) return false;
  const size_t rowBytes = resolvedWidth * 4u;
  const size_t stride = static_cast<size_t>(strideBytes);
  if (stride < rowBytes) return false;
  const size_t precedingRows = static_cast<size_t>(height - 1u);
  if (precedingRows > ((std::numeric_limits<size_t>::max)() - rowBytes) /
                          stride) {
    return false;
  }
  *bytes = precedingRows * stride + rowBytes;
  return true;
}

inline bool validate(const RgbaImageView& image,
                     size_t* required = nullptr) {
  size_t bytes = 0;
  if (!image.pixels ||
      !requiredBytes(image.width, image.height, image.strideBytes, &bytes) ||
      image.sizeBytes < bytes) {
    return false;
  }
  if (required) *required = bytes;
  return true;
}

inline RgbaImageView view(const RgbaImage& image) {
  return RgbaImageView{image.width, image.height, image.strideBytes,
                       image.pixels.data(), image.pixels.size()};
}

inline bool validate(const RgbaImage& image, size_t* required = nullptr) {
  return validate(view(image), required);
}

}  // namespace playback_video_image
