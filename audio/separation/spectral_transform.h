#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace audio_separation {

class BanditSpectralTransform {
 public:
  static constexpr std::size_t kFftSize = 2048;
  static constexpr std::size_t kHopFrames = 512;
  static constexpr std::size_t kFrequencyBins = kFftSize / 2 + 1;
  static constexpr std::size_t kChunkFrames = 8 * 48000;
  static constexpr std::size_t kTimeFrames =
      (kChunkFrames + 2 * (kFftSize / 2) - kFftSize) / kHopFrames + 1;
  static constexpr std::size_t kComplexValues =
      kFrequencyBins * kTimeFrames;
  static constexpr std::size_t kRealImagValues = 2 * kComplexValues;

  BanditSpectralTransform();
  ~BanditSpectralTransform();

  BanditSpectralTransform(const BanditSpectralTransform&) = delete;
  BanditSpectralTransform& operator=(const BanditSpectralTransform&) = delete;

  bool initialize(std::string* error);
  bool forward(const float* samples, std::size_t sampleCount,
               std::vector<float>* spectrogramRealImag,
               std::string* error);
  bool inverseMasked(const std::vector<float>& spectrogramRealImag,
                     const float* maskRealImag,
                     std::vector<float>* samples,
                     std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace audio_separation
