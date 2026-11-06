#ifndef RADIOIFY_AUDIOFILTER_MODELS_THERMIONIC_DIODE_H
#define RADIOIFY_AUDIOFILTER_MODELS_THERMIONIC_DIODE_H

#include <algorithm>
#include <cmath>

// Compact hot-cathode characteristic: Child-Langmuir space charge at positive
// voltage, joined C1 to a Maxwellian retarding-field tail. This is a reduced
// finite-temperature model, not a semiconductor Shockley law or a full
// Langmuir-Fry solution. Perveance is A/V^(3/2); thermal voltage is kT/e.
struct ThermionicDiode {
  float perveance = 0.00045;
  float thermalVolts = 0.073;
  float contactPotentialVolts = 0;

  struct Evaluation {
    double current;
    double conductance;
  };

  Evaluation evaluate(double anodeCathodeVolts) const {
    const double v = anodeCathodeVolts + contactPotentialVolts;
    const double join = 1.5 * thermalVolts;
    if (v >= join) {
      const double root = std::sqrt(v);
      return {perveance * v * root, 1.5 * perveance * root};
    }
    const double exponent = (v - join) / thermalVolts;
    // Below this point the current is below 5e-23 of the joining current.
    if (exponent < -52) return {0, 0};
    const double i = perveance * join * std::sqrt(join) * std::exp(exponent);
    return {i, i / thermalVolts};
  }

  // Diode in series with a Thevenin source, including the capacitor companion
  // resistance where applicable. Monotonicity gives a bracket; Newton starts
  // above the root and stays there because the characteristic is convex.
  double current(double sourceVolts, double resistanceOhms,
                 int* iterations = nullptr) const {
    const auto open = evaluate(sourceVolts);
    if (open.current == 0) return 0;
    double v = sourceVolts;
    if (sourceVolts + contactPotentialVolts > 0) {
      const double currentBound =
          std::max(sourceVolts + contactPotentialVolts, double(thermalVolts)) /
          resistanceOhms;
      v = std::min(v,
                   std::pow(currentBound / perveance, 2.0 / 3.0) - contactPotentialVolts);
    }
    for (int n = 0; n < 12; ++n) {
      const auto e = evaluate(v);
      const double delta = (v + resistanceOhms * e.current - sourceVolts) /
                           (1 + resistanceOhms * e.conductance);
      v -= delta;
      if (iterations) ++*iterations;
      if (std::abs(delta) < 1e-10 * (1 + std::abs(v))) break;
    }
    return evaluate(v).current;
  }
};

#endif
