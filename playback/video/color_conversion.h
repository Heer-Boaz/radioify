#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "playback/video/color.h"

namespace playback_video_color {

inline constexpr float kHdrReferenceNits = 100.0f;
inline constexpr float kPqNominalPeakNits = 10000.0f;
inline constexpr float kHdrToSdrScale =
    kPqNominalPeakNits / kHdrReferenceNits;

namespace detail {

inline float clamp(float value, float low, float high) {
  return value < low ? low : (value > high ? high : value);
}

inline uint8_t byte(float value) {
  if (value <= 0.0f) return 0;
  if (value >= 255.0f) return 255;
  return static_cast<uint8_t>(value + 0.5f);
}

inline float expandLuma(int value, bool fullRange, int bitDepth) {
  bitDepth = std::max(8, std::min(16, bitDepth));
  const int maxCode = (1 << bitDepth) - 1;
  value = std::max(0, std::min(maxCode, value));
  const int shift = bitDepth - 8;
  const float low = fullRange ? 0.0f : static_cast<float>(16 << shift);
  const float high = fullRange ? static_cast<float>(maxCode)
                               : static_cast<float>(235 << shift);
  return clamp((static_cast<float>(value) - low) /
                   std::max(1.0f, high - low),
               0.0f, 1.0f);
}

inline float expandChroma(int value, bool fullRange, int bitDepth) {
  bitDepth = std::max(8, std::min(16, bitDepth));
  const int maxCode = (1 << bitDepth) - 1;
  value = std::max(0, std::min(maxCode, value));
  const int shift = bitDepth - 8;
  const float low = fullRange ? 0.0f : static_cast<float>(16 << shift);
  const float high = fullRange ? static_cast<float>(maxCode)
                               : static_cast<float>(240 << shift);
  const float middle = static_cast<float>(128 << shift);
  return (static_cast<float>(value) - middle) /
         std::max(1.0f, high - low);
}

inline float pqEotf(float value) {
  constexpr float m1 = 2610.0f / 16384.0f;
  constexpr float m2 = 2523.0f / 32.0f;
  constexpr float c1 = 3424.0f / 4096.0f;
  constexpr float c2 = 2413.0f / 128.0f;
  constexpr float c3 = 2392.0f / 128.0f;
  const float powered = std::pow(std::max(value, 0.0f), 1.0f / m2);
  const float numerator = std::max(powered - c1, 0.0f);
  const float denominator = std::max(c2 - c3 * powered, 1e-6f);
  return std::pow(numerator / denominator, 1.0f / m1);
}

inline float hlgEotf(float value) {
  constexpr float a = 0.17883277f;
  constexpr float b = 1.0f - 4.0f * a;
  const float c = 0.5f - a * std::log(4.0f * a);
  value = clamp(value, 0.0f, 1.0f);
  if (value <= 0.5f) return (value * value) / 3.0f;
  return (std::exp((value - c) / a) + b) / 12.0f;
}

inline float filmic(float value) {
  constexpr float a = 0.15f;
  constexpr float b = 0.50f;
  constexpr float c = 0.10f;
  constexpr float d = 0.20f;
  constexpr float e = 0.02f;
  constexpr float f = 0.30f;
  return ((value * (a * value + c * b) + d * e) /
          (value * (a * value + b) + d * f)) -
         e / f;
}

inline float linearToSrgb(float value) {
  value = std::max(value, 0.0f);
  if (value <= 0.0031308f) return value * 12.92f;
  return 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

inline float transferToSrgb(float value, YuvTransfer transfer) {
  if (transfer == YuvTransfer::Sdr) return value;
  value = clamp(value, 0.0f, 1.0f);
  const float linear = transfer == YuvTransfer::Pq ? pqEotf(value)
                                                   : hlgEotf(value);
  return linearToSrgb(clamp(filmic(linear * kHdrToSdrScale), 0.0f, 1.0f));
}

}  // namespace detail

inline uint8_t lumaToSrgb8(int y, bool fullRange, int bitDepth,
                           YuvTransfer transfer) {
  return detail::byte(detail::clamp(
                          detail::transferToSrgb(
                              detail::expandLuma(y, fullRange, bitDepth),
                              transfer),
                          0.0f, 1.0f) *
                      255.0f);
}

inline void yuvToRgb8(int y, int u, int v, bool fullRange, int bitDepth,
                      YuvMatrix matrix, YuvTransfer transfer, uint8_t& red,
                      uint8_t& green, uint8_t& blue) {
  const float luma = detail::expandLuma(y, fullRange, bitDepth);
  const float chromaU = detail::expandChroma(u, fullRange, bitDepth);
  const float chromaV = detail::expandChroma(v, fullRange, bitDepth);

  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  if (matrix == YuvMatrix::Bt2020) {
    r = luma + 1.4746f * chromaV;
    g = luma - 0.16455f * chromaU - 0.57135f * chromaV;
    b = luma + 1.8814f * chromaU;
  } else if (matrix == YuvMatrix::Bt601) {
    r = luma + 1.4020f * chromaV;
    g = luma - 0.3441f * chromaU - 0.7141f * chromaV;
    b = luma + 1.7720f * chromaU;
  } else {
    r = luma + 1.5748f * chromaV;
    g = luma - 0.1873f * chromaU - 0.4681f * chromaV;
    b = luma + 1.8556f * chromaU;
  }

  r = detail::transferToSrgb(r, transfer);
  g = detail::transferToSrgb(g, transfer);
  b = detail::transferToSrgb(b, transfer);
  red = detail::byte(detail::clamp(r, 0.0f, 1.0f) * 255.0f);
  green = detail::byte(detail::clamp(g, 0.0f, 1.0f) * 255.0f);
  blue = detail::byte(detail::clamp(b, 0.0f, 1.0f) * 255.0f);
}

}  // namespace playback_video_color
