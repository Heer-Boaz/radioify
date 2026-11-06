#include "oversampling.h"

#include <cmath>
#include <stdexcept>

bool Oversampler::validConfiguration(float factor, float passbandFraction) {
  return (factor == 1 || factor == 2 || factor == 4 || factor == 8) &&
         std::isfinite(passbandFraction) && passbandFraction >= .05f &&
         passbandFraction <= .49f;
}

void Oversampler::configure(float factor, float passbandFraction) {
  if (!validConfiguration(factor, passbandFraction))
    throw std::invalid_argument(
        "Oversampling requires factor 1, 2, 4 or 8 and "
        "passband fraction .05..49.");
  factor_ = static_cast<int>(factor);
  stages_ = 0;
  for (int f = factor_; f > 1; f /= 2)
    ++stages_;
  coefficients_ = {};
  if (stages_ > 0) {
    // Half-band centre is host Nyquist. The legacy cutoff setting now defines
    // the flat passband edge, relative to the input rate of each 2x stage.
    const double transition = .5 - passbandFraction;
    coefficients_.configure(transition);
  }
  reset();
}

void Oversampler::reset() {
  filters_ = {};
  samples_ = {};
}
