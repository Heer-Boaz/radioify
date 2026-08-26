#include "audio/separation/spectral_transform.h"

extern "C" {
#include <libavutil/tx.h>
}

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace audio_separation {
namespace {

constexpr std::size_t kCenterPad = BanditSpectralTransform::kFftSize / 2;
constexpr std::size_t kPaddedFrames =
    BanditSpectralTransform::kChunkFrames + 2 * kCenterPad;

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

float reflectedSample(const float* samples, std::size_t paddedIndex) {
  if (paddedIndex < kCenterPad) {
    return samples[kCenterPad - paddedIndex];
  }
  const std::size_t sourceIndex = paddedIndex - kCenterPad;
  if (sourceIndex < BanditSpectralTransform::kChunkFrames) {
    return samples[sourceIndex];
  }
  const std::size_t rightIndex =
      sourceIndex - BanditSpectralTransform::kChunkFrames;
  return samples[BanditSpectralTransform::kChunkFrames - 2 - rightIndex];
}

}  // namespace

struct BanditSpectralTransform::Impl {
  AVTXContext* forwardContext = nullptr;
  AVTXContext* inverseContext = nullptr;
  av_tx_fn forwardFunction = nullptr;
  av_tx_fn inverseFunction = nullptr;
  std::vector<float> window;
  std::vector<float> denominator;
  std::vector<float> timeInput;
  std::vector<float> timeOutput;
  std::vector<AVComplexFloat> frequencyValues;
  std::vector<float> overlap;

  ~Impl() {
    av_tx_uninit(&forwardContext);
    av_tx_uninit(&inverseContext);
  }
};

BanditSpectralTransform::BanditSpectralTransform() = default;
BanditSpectralTransform::~BanditSpectralTransform() = default;

bool BanditSpectralTransform::initialize(std::string* error) {
  if (error) error->clear();
  auto implementation = std::make_unique<Impl>();
  // BandIt was trained with torchaudio's normalized=True contract. That is
  // an orthonormal DFT in both directions; the Hann window itself is not
  // normalized.
  const float transformScale =
      1.0f / static_cast<float>(std::sqrt(static_cast<double>(kFftSize)));
  if (av_tx_init(&implementation->forwardContext,
                 &implementation->forwardFunction, AV_TX_FLOAT_RDFT, 0,
                 static_cast<int>(kFftSize), &transformScale,
                 AV_TX_UNALIGNED) < 0 ||
      av_tx_init(&implementation->inverseContext,
                 &implementation->inverseFunction, AV_TX_FLOAT_RDFT, 1,
                 static_cast<int>(kFftSize), &transformScale,
                 AV_TX_UNALIGNED) < 0) {
    setError(error, "Could not initialize the BandIt Fourier transforms.");
    return false;
  }

  implementation->window.resize(kFftSize);
  for (std::size_t index = 0; index < kFftSize; ++index) {
    const double angle =
        2.0 * std::numbers::pi * static_cast<double>(index) /
        static_cast<double>(kFftSize);
    const double value = 0.5 - 0.5 * std::cos(angle);
    implementation->window[index] = static_cast<float>(value);
  }

  implementation->denominator.assign(kPaddedFrames, 0.0f);
  for (std::size_t time = 0; time < kTimeFrames; ++time) {
    const std::size_t start = time * kHopFrames;
    for (std::size_t index = 0; index < kFftSize; ++index) {
      const float value = implementation->window[index];
      implementation->denominator[start + index] += value * value;
    }
  }
  implementation->timeInput.resize(kFftSize);
  implementation->timeOutput.resize(kFftSize);
  implementation->frequencyValues.resize(kFrequencyBins);
  implementation->overlap.resize(kPaddedFrames);
  impl_ = std::move(implementation);
  return true;
}

bool BanditSpectralTransform::forward(
    const float* samples, std::size_t sampleCount,
    std::vector<float>* spectrogramRealImag, std::string* error) {
  if (!impl_ || !samples || !spectrogramRealImag ||
      sampleCount != kChunkFrames) {
    setError(error, "The BandIt STFT received an invalid audio chunk.");
    return false;
  }
  spectrogramRealImag->assign(kRealImagValues, 0.0f);
  for (std::size_t time = 0; time < kTimeFrames; ++time) {
    const std::size_t start = time * kHopFrames;
    for (std::size_t index = 0; index < kFftSize; ++index) {
      impl_->timeInput[index] =
          reflectedSample(samples, start + index) * impl_->window[index];
    }
    impl_->forwardFunction(impl_->forwardContext,
                           impl_->frequencyValues.data(),
                           impl_->timeInput.data(), sizeof(float));
    for (std::size_t frequency = 0; frequency < kFrequencyBins;
         ++frequency) {
      const std::size_t outputIndex =
          2 * (frequency * kTimeFrames + time);
      (*spectrogramRealImag)[outputIndex] =
          impl_->frequencyValues[frequency].re;
      (*spectrogramRealImag)[outputIndex + 1] =
          impl_->frequencyValues[frequency].im;
    }
  }
  return true;
}

bool BanditSpectralTransform::inverseMasked(
    const std::vector<float>& spectrogramRealImag,
    const float* maskRealImag, std::vector<float>* samples,
    std::string* error) {
  if (!impl_ || !maskRealImag || !samples ||
      spectrogramRealImag.size() != kRealImagValues) {
    setError(error, "The BandIt ISTFT received an invalid mask.");
    return false;
  }
  std::fill(impl_->overlap.begin(), impl_->overlap.end(), 0.0f);
  for (std::size_t time = 0; time < kTimeFrames; ++time) {
    for (std::size_t frequency = 0; frequency < kFrequencyBins;
         ++frequency) {
      const std::size_t valueIndex =
          2 * (frequency * kTimeFrames + time);
      const float sourceReal = spectrogramRealImag[valueIndex];
      const float sourceImag = spectrogramRealImag[valueIndex + 1];
      const float maskReal = maskRealImag[valueIndex];
      const float maskImag = maskRealImag[valueIndex + 1];
      impl_->frequencyValues[frequency] = {
          sourceReal * maskReal - sourceImag * maskImag,
          sourceReal * maskImag + sourceImag * maskReal};
    }
    impl_->inverseFunction(impl_->inverseContext, impl_->timeOutput.data(),
                           impl_->frequencyValues.data(),
                           sizeof(AVComplexFloat));
    const std::size_t start = time * kHopFrames;
    for (std::size_t index = 0; index < kFftSize; ++index) {
      impl_->overlap[start + index] +=
          impl_->timeOutput[index] * impl_->window[index];
    }
  }

  samples->resize(kChunkFrames);
  for (std::size_t index = 0; index < kChunkFrames; ++index) {
    const std::size_t paddedIndex = index + kCenterPad;
    const float denominator = impl_->denominator[paddedIndex];
    const float value = denominator > 1.0e-12f
                            ? impl_->overlap[paddedIndex] / denominator
                            : 0.0f;
    (*samples)[index] = std::isfinite(value) ? value : 0.0f;
  }
  return true;
}

}  // namespace audio_separation
