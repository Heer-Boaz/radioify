#include "remote_cutoff_pentode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <stdexcept>

namespace {

struct Point {
  double gridVolts;
  double micromhos;
};

// RCA-78 average characteristics, graph 92S-5248 (April 5, 1937), Eb 250 V,
// Ec2 100 V. -3 V is RCA's 1450 umho rating; -4 to -22 V is a two-exponential
// fit to graphical readings (within 10% of each reading); below -22 V the
// curve merges into the axis and is extrapolated to its visible cutoff near
// -40 V. These are type-family values, not a measured specimen.
constexpr std::array<Point, 20> kCurve{{
    {-2, 1750}, {-3, 1450}, {-4, 1174}, {-5, 907}, {-6, 703},  {-7, 548},  {-8, 429},
    {-10, 270}, {-12, 176}, {-14, 120}, {-16, 87}, {-18, 67}, {-20, 54},  {-22, 45},
    {-25, 33},  {-30, 19},  {-35, 9},   {-40, 4},  {-45, 2},  {-50, 1},
}};

// Log-linear interpolation; the end segments extend their slope.
double logMicromhos(double gridVolts) {
  auto upper = std::find_if(kCurve.begin(), kCurve.end(),
                            [&](const Point& p) { return p.gridVolts <= gridVolts; });
  if (upper == kCurve.begin()) ++upper;
  if (upper == kCurve.end()) --upper;
  const Point& hi = *std::prev(upper);
  const Point& lo = *upper;
  const double t = (gridVolts - lo.gridVolts) / (hi.gridVolts - lo.gridVolts);
  return std::log(lo.micromhos) + t * (std::log(hi.micromhos) - std::log(lo.micromhos));
}

}  // namespace

double remoteCutoffTransconductance(double gridVolts) {
  if (!std::isfinite(gridVolts)) throw std::invalid_argument("grid volts must be finite");
  return 1e-6 * std::exp(logMicromhos(gridVolts));
}

double remoteCutoffRelativeGain(double biasVolts, double avcVolts, double screenVolts) {
  if (!std::isfinite(biasVolts) || !std::isfinite(avcVolts) || !std::isfinite(screenVolts) ||
      screenVolts <= 0)
    throw std::invalid_argument("remote-cutoff stage needs finite bias/AVC and screen > 0");
  const double scale = 100 / screenVolts;
  return std::exp(logMicromhos(scale * (biasVolts - std::max(avcVolts, 0.0))) -
                  logMicromhos(scale * biasVolts));
}
