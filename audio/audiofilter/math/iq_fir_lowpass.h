#ifndef RADIOIFY_AUDIOFILTER_MATH_IQ_FIR_LOWPASS_H
#define RADIOIFY_AUDIOFILTER_MATH_IQ_FIR_LOWPASS_H

#include <array>

// Numerical image rejection for quadrature downmix. Both channels share a
// linear-phase coefficient set. Configuration preserves history during tuning;
// reset is explicit. The streaming path uses fixed storage and no allocation.
class IQFirLowpass {
 public:
  void configure(double passbandFraction, double stopbandFraction);
  // Cold-path frequency response, e.g. for transmitter noise calibration.
  double magnitudeSquared(double radians) const;
  void reset() {
    history_ = {};
    position_ = 0;
  }
  static constexpr int delaySamples() { return (kMaxTaps - 1) / 2; }
  std::array<float, 2> process(float i, float q) {
    position_ = (position_ - 1) & (kCapacity - 1);
    history_[position_] = history_[position_ + kCapacity] = {i, q};
    double outI = 0, outQ = 0;
    const auto* samples = history_.data() + position_ + firstTap_;
    const int middle = count_ / 2;
    // Linear-phase symmetry halves the number of coefficient multiplies.
    for (int tap = 0; tap < middle; ++tap) {
      const auto& left = samples[tap];
      const auto& right = samples[count_ - 1 - tap];
      outI += coefficients_[tap] * (static_cast<double>(left[0]) + right[0]);
      outQ += coefficients_[tap] * (static_cast<double>(left[1]) + right[1]);
    }
    outI += coefficients_[middle] * samples[middle][0];
    outQ += coefficients_[middle] * samples[middle][1];
    return {static_cast<float>(outI), static_cast<float>(outQ)};
  }

 private:
  static constexpr int kCapacity = 128;
  static constexpr int kMaxTaps = 69;
  std::array<double, kMaxTaps> coefficients_{};
  std::array<std::array<float, 2>, 2 * kCapacity> history_{};
  int count_ = 1, firstTap_ = delaySamples(), position_ = 0;
};

#endif
