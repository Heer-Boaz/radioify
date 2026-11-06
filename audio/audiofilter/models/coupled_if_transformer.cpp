#include "coupled_if_transformer.h"

#include <cmath>
#include <stdexcept>

void CoupledIfTransformer::configure(double sampleRate, double centreHz, double lp,
                                     double ls, double rp, double rs, double load,
                                     double k, double reference) {
  for (double v : {sampleRate, centreHz, lp, ls, rp, rs, load, reference}) {
    if (!std::isfinite(v) || v <= 0)
      throw std::invalid_argument("IF tank values must be finite and positive");
  }
  if (!std::isfinite(k) || k < 0 || k >= 1)
    throw std::invalid_argument("IF magnetic coupling must be in [0, 1)");
  constexpr double pi = 3.14159265358979323846;
  const double omega = 2 * pi * centreHz;
  // The trimmers align both tanks to centreHz. L therefore changes loss/Q
  // and the voltage ratio, rather than silently detuning an aligned receiver.
  const double cp = 1 / (omega * omega * lp);
  const double cs = 1 / (omega * omega * ls);
  const double a = rp / (2 * lp);
  const double b = rs / (2 * ls) + 1 / (2 * cs * load);
  const double g = omega * k / 2;
  const double g01 = g * std::sqrt(lp / ls);
  const double g10 = g * std::sqrt(ls / lp);
  const double h = .5 / sampleRate;
  // Trapezoidal integration: (I-hA)^-1 [(I+hA)x + hB(u[n]+u[n-1])].
  const double determinant = (1 + h * a) * (1 + h * b) + h * h * g01 * g10;
  const double r00 = (1 + h * b) / determinant;
  const double r01 = -h * g01 / determinant;
  const double r10 = h * g10 / determinant;
  const double r11 = (1 + h * a) / determinant;
  m00_ = 2 * r00 - 1;
  m01_ = 2 * r01;
  m10_ = 2 * r10;
  m11_ = 2 * r11 - 1;
  // This reference converts the input envelope's units to current. It is a
  // declared gain calibration, not an extra loading resistor or per-curve AGC.
  const double drive = h / (2 * cp * reference);
  b0_ = r00 * drive;
  b1_ = r10 * drive;
}

std::array<float, 2> CoupledIfTransformer::process(float i, float q) {
  const std::array<double, 2> input{i, q};
  std::array<float, 2> out{};
  for (int n = 0; n < 2; ++n) {
    const double u = input[n] + previousInput_[n];
    const double primary = m00_ * primary_[n] + m01_ * secondary_[n] + b0_ * u;
    secondary_[n] = m10_ * primary_[n] + m11_ * secondary_[n] + b1_ * u;
    primary_[n] = primary;
    previousInput_[n] = input[n];
    out[n] = static_cast<float>(secondary_[n]);
  }
  return out;
}

void CoupledIfTransformer::reset() {
  primary_ = {};
  secondary_ = {};
  previousInput_ = {};
}
