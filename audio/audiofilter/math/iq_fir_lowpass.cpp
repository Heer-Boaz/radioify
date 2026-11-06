#include "iq_fir_lowpass.h"

#include <cmath>
#include <algorithm>
#include <stdexcept>

double IQFirLowpass::magnitudeSquared(double radians) const {
  const int middle = count_ / 2;
  double value = coefficients_[middle];
  for (int i = 0; i < middle; ++i)
    value += 2 * coefficients_[i] * std::cos(radians * (middle - i));
  return value * value;
}

void IQFirLowpass::configure(double passbandFraction, double stopbandFraction) {
  if (!(passbandFraction > 0 && stopbandFraction > passbandFraction &&
        stopbandFraction < .5))
    throw std::invalid_argument("IQ image filter requires 0 < pass < stop < Nyquist.");
  constexpr double pi = 3.14159265358979323846;
  constexpr double attenuation = 100;
  constexpr double beta = .1102 * (attenuation - 8.7);
  const double transition = stopbandFraction - passbandFraction;
  // Kaiser window order estimate, rounded up to even order with two extra taps
  // for margin. Tuning reserves >=.1*fs between pass and stop, so storage is fixed.
  const double estimated = (attenuation - 8) / (2.285 * 2 * pi * transition);
  if (estimated > kMaxTaps - 3)
    throw std::invalid_argument("IQ image transition exceeds filter capacity.");
  const int order = std::max(40, 2 * static_cast<int>(std::ceil(estimated / 2)) + 2);
  count_ = order + 1;
  // Pad shorter designs equally at both ends. Bandwidth changes must not
  // move the audio by whole samples when the required filter order changes.
  firstTap_ = (kMaxTaps - count_) / 2;
  const double cutoff = .5 * (passbandFraction + stopbandFraction);
  const double windowScale = std::cyl_bessel_i(0.0, beta);
  double sum = 0;
  for (int tap = 0; tap < count_; ++tap) {
    const int offset = tap - order / 2;
    const double sinc =
        offset ? std::sin(2 * pi * cutoff * offset) / (pi * offset) : 2 * cutoff;
    const double radius = 2.0 * offset / order;
    const double window =
        std::cyl_bessel_i(0.0, beta * std::sqrt(1 - radius * radius)) / windowScale;
    coefficients_[tap] = sinc * window;
    sum += coefficients_[tap];
  }
  for (int tap = 0; tap < count_; ++tap) coefficients_[tap] /= sum;
}
