#ifndef RADIOIFY_AUDIOFILTER_MATH_HALFBAND_IIR_H
#define RADIOIFY_AUDIOFILTER_MATH_HALFBAND_IIR_H

#include <array>

// Coefficients are designed at initialization/configuration boundaries only.
struct HalfbandIirCoefficients {
  static constexpr int kCapacity = 12;
  std::array<double, kCapacity> values{};
  int count = 0;
  void configure(double transition);
};

struct HalfbandAllpassPair {
  std::array<double, HalfbandIirCoefficients::kCapacity> previousInput{},
      previousOutput{};
  void process(double& even, double& odd, const HalfbandIirCoefficients& coefficients) {
    // A(z) = (a + z^-1)/(1 + a*z^-1), evaluated at the polyphase rate.
    for (int i = 0; i < coefficients.count; ++i) {
      double& x = (i & 1) ? odd : even;
      const double y =
          coefficients.values[i] * (x - previousOutput[i]) + previousInput[i];
      previousInput[i] = x;
      previousOutput[i] = y;
      x = y;
    }
  }
};

#endif
