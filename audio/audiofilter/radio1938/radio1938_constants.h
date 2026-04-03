#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_RADIO1938_CONSTANTS_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_RADIO1938_CONSTANTS_H

#include <array>
#include <cmath>
#include <cstddef>

inline constexpr float kRadioPi = 3.1415926535f;
inline constexpr float kRadioTwoPi = 6.283185307f;
inline constexpr float kRadioBiquadQ = 0.707f;
inline constexpr float kRadioLinDbFloor = 1e-12f;
inline constexpr float kRadioSoftClipThresholdDefault = 0.98f;
inline constexpr float kRadioHashUnitInv = 1.0f / 4294967295.0f;
inline constexpr size_t kRadioCalibrationBandCount = 12;
inline constexpr size_t kRadioCalibrationFftSize = 1024;
inline constexpr size_t kRadioCalibrationFftBinCount =
    kRadioCalibrationFftSize / 2 + 1;
inline constexpr std::array<float, kRadioCalibrationBandCount>
    kRadioCalibrationBandHz{{125.0f, 250.0f, 400.0f, 630.0f, 1000.0f, 1250.0f,
                             1600.0f, 2000.0f, 2500.0f, 3150.0f, 4000.0f,
                             5000.0f}};

inline const std::array<float, kRadioCalibrationBandCount + 1>&
radioCalibrationBandEdgesHz() {
  static const auto kEdges = [] {
    std::array<float, kRadioCalibrationBandCount + 1> edges{};
    edges[0] = kRadioCalibrationBandHz[0] /
               std::sqrt(kRadioCalibrationBandHz[1] / kRadioCalibrationBandHz[0]);
    for (size_t i = 1; i < kRadioCalibrationBandCount; ++i) {
      edges[i] = std::sqrt(kRadioCalibrationBandHz[i - 1] *
                           kRadioCalibrationBandHz[i]);
    }
    edges[kRadioCalibrationBandCount] =
        kRadioCalibrationBandHz.back() *
        std::sqrt(kRadioCalibrationBandHz.back() /
                  kRadioCalibrationBandHz[kRadioCalibrationBandCount - 2]);
    return edges;
  }();
  return kEdges;
}

inline const std::array<float, kRadioCalibrationFftSize>&
radioCalibrationWindow() {
  static const auto kWindow = [] {
    std::array<float, kRadioCalibrationFftSize> window{};
    for (size_t i = 0; i < kRadioCalibrationFftSize; ++i) {
      window[i] = 0.5f - 0.5f * std::cos(kRadioTwoPi * static_cast<float>(i) /
                                         static_cast<float>(window.size() - 1));
    }
    return window;
  }();
  return kWindow;
}

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_RADIO1938_CONSTANTS_H
