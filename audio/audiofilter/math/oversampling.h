#ifndef RADIOIFY_AUDIOFILTER_MATH_OVERSAMPLING_H
#define RADIOIFY_AUDIOFILTER_MATH_OVERSAMPLING_H

#include <array>

#include "halfband_iir.h"

// Streaming 1x/2x/4x/8x nonlinear island. Each rate change has its own
// interpolation and decimation histories. No allocation or coefficient design
// occurs on the sample path. Coefficients and actual callback rate share one
// configuration, so a stage cannot accidentally run at a different rate.
class Oversampler {
 public:
  static constexpr int kMaxFactor = 8;
  static bool validConfiguration(float factor, float passbandFraction);
  void configure(float factor, float passbandFraction);
  void reset();
  int factor() const { return factor_; }

  const std::array<float, kMaxFactor>& upsample(float x) {
    samples_[0] = x;
    for (int stage = 0, count = 1; stage < stages_; ++stage, count *= 2) {
      std::array<float, kMaxFactor> next{};
      for (int i = 0; i < count; ++i) {
        double even = samples_[i], odd = even;
        filters_[stage].up.process(even, odd, coefficients_);
        next[2 * i] = static_cast<float>(even);
        next[2 * i + 1] = static_cast<float>(odd);
      }
      samples_ = next;
    }
    return samples_;
  }

  float downsample(const std::array<float, kMaxFactor>& input) {
    samples_ = input;
    for (int stage = stages_ - 1, count = factor_; stage >= 0; --stage, count /= 2) {
      for (int i = 0; i < count / 2; ++i) {
        // Reversed polyphase order accounts for the odd branch's unit delay.
        double even = samples_[2 * i + 1], odd = samples_[2 * i];
        filters_[stage].down.process(even, odd, coefficients_);
        samples_[i] = static_cast<float>(.5 * (even + odd));
      }
    }
    return samples_[0];
  }

  template <typename Process>
  float process(float x, Process&& processSample) {
    if (factor_ == 1)
      return processSample(x);
    upsample(x);
    for (int i = 0; i < factor_; ++i)
      samples_[i] = processSample(samples_[i]);
    return downsample(samples_);
  }

 private:
  struct Halfband {
    HalfbandAllpassPair up, down;
  };
  int factor_ = 1, stages_ = 0;
  HalfbandIirCoefficients coefficients_;
  std::array<Halfband, 3> filters_{};
  std::array<float, kMaxFactor> samples_{};
};

#endif
