#include "resonant_networks.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <stdexcept>

namespace {

constexpr double kPi = 3.14159265358979323846;

// Monic minimum-phase factor {1, g1, g2} of p0 + p1*c + p2*c^2, c = cos(w),
// where p is positive on [-1, 1]. Each root r in c gives a zero rho with
// |1 - rho e^-jw|^2 proportional to c - r; it is solved from u = 1/r so a
// vanishing p2 (a first-order factor) needs no special case.
std::array<double, 3> minimumPhaseFactor(double p0, double p1, double p2) {
  using Complex = std::complex<double>;
  const Complex root = std::sqrt(Complex(p1 * p1 - 4 * p2 * p0));
  const Complex q = -.5 * (p1 + (p1 >= 0 ? root : -root));
  const auto zero = [](Complex u) {
    if (std::abs(u) == 0) return Complex{};
    const Complex s = std::sqrt(1. - u * u);
    const Complex inner = (1. - s) / u, outer = (1. + s) / u;
    return std::abs(inner) < std::abs(outer) ? inner : outer;
  };
  const Complex rho1 = zero(p2 / q), rho2 = zero(q / p0);
  if (!(std::abs(rho1) < 1 && std::abs(rho2) < 1))
    throw std::runtime_error("RF tank magnitude fit is not realisable");
  return {1, -(rho1 + rho2).real(), (rho1 * rho2).real()};
}

}  // namespace

void TunedRfCircuit::configure(double sampleRate, double dialHz, double inductanceHenries,
                               double coilQ, double shuntLoadOhms) {
  for (double v : {sampleRate, dialHz, inductanceHenries, coilQ, shuntLoadOhms}) {
    if (!std::isfinite(v) || v <= 0)
      throw std::invalid_argument("RF tank values must be finite and positive");
  }
  const double omega = 2 * kPi * dialHz;
  loadedQ_ = 1 / (1 / coilQ + omega * inductanceHenries / shuntLoadOhms);
  const double alpha = omega / (2 * loadedQ_);
  halfBandwidthHz_ = alpha / (2 * kPi);

  // |H|^2 = 1/(1 + (f/B)^2) = N(c)/D(c) with quadratic N and D, D(0) = 1,
  // solved through five frequencies by Gaussian elimination.
  constexpr double fractions[]{0, .1, .2, .3, .5};
  double m[5][6];
  for (int row = 0; row < 5; ++row) {
    const double c = std::cos(2 * kPi * fractions[row]);
    const double x = fractions[row] * sampleRate / halfBandwidthHz_;
    const double t = 1 / (1 + x * x);
    const double values[]{1, c, c * c, -t * c, -t * c * c, t};
    std::copy(std::begin(values), std::end(values), m[row]);
  }
  for (int col = 0; col < 5; ++col) {
    int pivot = col;
    for (int row = col + 1; row < 5; ++row)
      if (std::abs(m[row][col]) > std::abs(m[pivot][col])) pivot = row;
    std::swap(m[col], m[pivot]);
    for (int row = 0; row < 5; ++row) {
      if (row == col) continue;
      const double f = m[row][col] / m[col][col];
      for (int k = col; k < 6; ++k) m[row][k] -= f * m[col][k];
    }
  }
  double p[5];
  for (int i = 0; i < 5; ++i) p[i] = m[i][5] / m[i][i];

  const auto numerator = minimumPhaseFactor(p[0], p[1], p[2]);
  const auto denominator = minimumPhaseFactor(1, p[3], p[4]);
  // Unity gain at resonance fixes the numerator scale.
  const double scale = (denominator[0] + denominator[1] + denominator[2]) /
                       (numerator[0] + numerator[1] + numerator[2]);
  for (int k = 0; k < 3; ++k) b_[k] = scale * numerator[k];
  a_ = {denominator[1], denominator[2]};
}

std::array<float, 2> TunedRfCircuit::process(float i, float q) {
  const std::array<double, 2> input{i, q};
  std::array<float, 2> out{};
  for (int n = 0; n < 2; ++n) {
    auto& s = state_[n];
    const double y = b_[0] * input[n] + s[0];
    s[0] = b_[1] * input[n] - a_[0] * y + s[1];
    s[1] = b_[2] * input[n] - a_[1] * y;
    out[n] = static_cast<float>(y);
  }
  return out;
}

void TunedRfCircuit::reset() { state_ = {}; }
