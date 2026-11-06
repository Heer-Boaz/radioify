#include "radio_room.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kSoundSpeed = 343.0;
constexpr double kHeadRadius = 0.0875;
constexpr float kInvSqrt8 = 0.3535533905932738f;

struct Vec {
  double x, y, z;
  Vec operator-(Vec b) const { return {x - b.x, y - b.y, z - b.z}; }
  double length() const { return std::sqrt(x * x + y * y + z * z); }
  double dot(Vec b) const { return x * b.x + y * b.y + z * b.z; }
};

double diskCdf(double u) {
  u = std::clamp(u, -1.0, 1.0);
  return 0.5 + (std::asin(u) + u * std::sqrt(std::max(0.0, 1.0 - u * u))) / kPi;
}

double headAlpha(double theta) {
  // Brown/Duda (1998), spherical-head component, alpha_min=.1, theta_min=150 deg.
  return 1.05 + 0.95 * std::cos(theta * kPi / (5.0 * kPi / 6.0));
}

double headDelay(double theta) {
  // Woodworth high-frequency delay relative to the head centre.
  return kHeadRadius / kSoundSpeed *
         (theta <= kPi / 2.0 ? -std::cos(theta) : theta - kPi / 2.0);
}

void configureHead(Biquad& filter, double theta, double sampleRate) {
  // Frontal equalization keeps an on-axis source's existing spectral balance.
  // H(theta)/H(90 deg) = (alpha(theta)*s + beta)/(alpha(90)*s + beta).
  const double beta = 2.0 * kSoundSpeed / kHeadRadius;
  const double a = headAlpha(theta);
  const double reference = headAlpha(kPi / 2.0);
  const double denominator = reference * 2.0 * sampleRate + beta;
  filter.b0 = static_cast<float>((a * 2.0 * sampleRate + beta) / denominator);
  filter.b1 = static_cast<float>((beta - a * 2.0 * sampleRate) / denominator);
  filter.a1 = static_cast<float>((beta - reference * 2.0 * sampleRate) / denominator);
}
}  // namespace

bool validateRadioRoomConfig(const RadioRoomConfig& c, std::string* error) {
  const auto bounded = [](float x, float lo, float hi) {
    return std::isfinite(x) && x >= lo && x <= hi;
  };
  const bool valid = bounded(c.reflections, 0, 1) && bounded(c.lateLevel, 0, 1) &&
                     bounded(c.decaySeconds, .05f, 3) && bounded(c.widthMeters, 1, 20) &&
                     bounded(c.lengthMeters, 1, 20) && bounded(c.heightMeters, 1, 10) &&
                     bounded(c.sourceXMeters, .1f, c.widthMeters - .1f) &&
                     bounded(c.sourceYMeters, .1f, c.lengthMeters - .1f) &&
                     bounded(c.sourceZMeters, .1f, c.heightMeters - .1f) &&
                     bounded(c.listenerXMeters, .1f, c.widthMeters - .1f) &&
                     bounded(c.listenerYMeters, .1f, c.lengthMeters - .1f) &&
                     bounded(c.listenerZMeters, .1f, c.heightMeters - .1f) &&
                     bounded(c.speakerYawDegrees, -180, 180) &&
                     bounded(c.headYawDegrees, -180, 180) &&
                     bounded(c.wallLowpassHz, 200, 16000) &&
                     (Vec{c.sourceXMeters, c.sourceYMeters, c.sourceZMeters} -
                      Vec{c.listenerXMeters, c.listenerYMeters, c.listenerZMeters})
                             .length() >= .5;
  if (!valid && error) {
    *error =
        "Invalid room: mix levels 0..1, decay .05..3 s, dimensions 1..20 m "
        "(height <=10), positions >=.1 m inside walls and >=.5 m apart, "
        "yaw -180..180 degrees, wallLowpassHz 200..16000.";
  }
  return valid;
}

void RadioRoomRenderer::initialize(const RadioRoomConfig& c, float fs, float radius) {
  assert(validateRadioRoomConfig(c, nullptr) && fs > 0 && radius > 0);
  enabled_ = c.enabled;
  headphones_ = c.headphones;
  for (auto& ear : paths_)
    for (auto& path : ear)
      path = Path{};
  history_.clear();
  for (auto& line : late_)
    line = ReverbLine{};
  if (!enabled_) {
    reset();
    return;
  }

  const Vec source{c.sourceXMeters, c.sourceYMeters, c.sourceZMeters};
  const Vec listener{c.listenerXMeters, c.listenerYMeters, c.listenerZMeters};
  const double yaw = c.speakerYawDegrees * kPi / 180.0;
  const Vec axis{std::sin(yaw), std::cos(yaw), 0};
  const double headYaw = c.headYawDegrees * kPi / 180.0;
  const Vec right{std::cos(headYaw), std::sin(headYaw), 0};
  std::array<Vec, 7> images{source,
                            {-source.x, source.y, source.z},
                            {2 * c.widthMeters - source.x, source.y, source.z},
                            {source.x, -source.y, source.z},
                            {source.x, 2 * c.lengthMeters - source.y, source.z},
                            {source.x, source.y, -source.z},
                            {source.x, source.y, 2 * c.heightMeters - source.z}};
  const double directDistance = (listener - source).length();
  const double volume = c.widthMeters * c.lengthMeters * c.heightMeters;
  const double surface =
      2.0 * (c.widthMeters * c.lengthMeters + c.widthMeters * c.heightMeters +
             c.lengthMeters * c.heightMeters);
  // Eyring diffuse-field approximation; the tail below is an artificial FDN,
  // not a claim to solve the room's low-frequency wave modes.
  const double reflection = std::exp(-.5 * .161 * volume / (surface * c.decaySeconds));
  struct Geometry {
    double centre, spread, theta, gain;
  };
  std::array<std::array<Geometry, 7>, 2> geometry{};
  double earliest = 1e12;
  for (size_t ear = 0; ear < 2; ++ear) {
    for (size_t p = 0; p < 7; ++p) {
      const Vec ray = listener - images[p];
      const double distance = ray.length();
      Vec imageAxis = axis;
      if (p == 1 || p == 2)
        imageAxis.x = -imageAxis.x;
      if (p == 3 || p == 4)
        imageAxis.y = -imageAxis.y;
      if (p == 5 || p == 6)
        imageAxis.z = -imageAxis.z;
      const double cosine = std::clamp(ray.dot(imageAxis) / distance, -1.0, 1.0);
      const double earCosine =
          std::clamp(-ray.dot(right) / distance * (ear == 0 ? -1.0 : 1.0), -1.0, 1.0);
      const double theta = std::acos(earCosine);
      const double spread =
          radius * std::sqrt(std::max(0.0, 1.0 - cosine * cosine)) / kSoundSpeed * fs;
      const double centre =
          (distance / kSoundSpeed + (headphones_ ? headDelay(theta) : 0.0)) * fs;
      // Ideal dipole angular factor times piston aperture directivity. The
      // on-axis cabinet response already includes its LF mounting loss.
      const double gain = cosine * directDistance / distance *
                          (p == 0 ? 1.0 : reflection * c.reflections);
      geometry[ear][p] = {centre, spread, theta, gain};
      earliest = std::min(earliest, centre - spread);
    }
  }
  size_t maxDelay = 0;
  double reflectionEnergy = 0.0;
  for (size_t ear = 0; ear < 2; ++ear) {
    for (size_t p = 0; p < 7; ++p) {
      auto& path = paths_[ear][p];
      const auto& g = geometry[ear][p];
      // Remove the shared flight time to preserve video sync. One sample of
      // causal guard supports the cubic fractional-delay interpolation.
      const double centre = g.centre - earliest + 1.0;
      path.gain = static_cast<float>(g.gain);
      if (p > 0) {
        path.wall.setLowpass(fs, std::min(c.wallLowpassHz, .4f * fs), .70710678f);
        reflectionEnergy += .5 * g.gain * g.gain;
      }
      if (headphones_)
        configureHead(path.head, g.theta, fs);
      if (g.spread < .5) {
        const auto base = static_cast<size_t>(std::floor(centre)) - 1;
        const double t = centre - base;
        for (size_t j = 0; j < 4; ++j) {
          double w = 1.0;
          for (size_t k = 0; k < 4; ++k)
            if (k != j)
              w *= (t - k) / (static_cast<double>(j) - k);
          path.taps.push_back({base + j, static_cast<float>(w)});
        }
      } else {
        // Projection of a uniformly driven disk is a semicircle. Its Fourier
        // transform is 2*J1(ka*sin(theta))/(ka*sin(theta)), including sidelobes.
        const auto first =
            static_cast<size_t>(std::max(0.0, std::floor(centre - g.spread - .5)));
        const auto last = static_cast<size_t>(std::ceil(centre + g.spread + .5));
        for (size_t n = first; n <= last; ++n) {
          const double w = diskCdf((n + .5 - centre) / g.spread) -
                           diskCdf((n - .5 - centre) / g.spread);
          if (w > 0)
            path.taps.push_back({n, static_cast<float>(w)});
        }
      }
      maxDelay = std::max(maxDelay, path.taps.back().delay);
    }
  }
  history_.assign(maxDelay + 1, 0.0f);
  lateLevel_ = c.reflections * c.lateLevel;
  outputGain_ = static_cast<float>(
      1.0 / std::sqrt(1.0 + reflectionEnergy + lateLevel_ * lateLevel_));
  // Incommensurate delay lengths scaled by room size. Orthogonal mixing and
  // strictly sub-unity damping keep the network passive and the tail bounded.
  constexpr double lengths[]{.0297, .0371, .0411, .0437, .0531, .0617, .0673, .0719};
  const double sizeScale = std::cbrt(volume / 52.0);
  for (size_t i = 0; i < late_.size(); ++i) {
    auto& line = late_[i];
    const size_t n = std::max<size_t>(
        1, static_cast<size_t>(std::lround(lengths[i] * sizeScale * fs)));
    const double seconds = static_cast<double>(n) / fs;
    line.buffer.assign(n, 0.0f);
    line.feedback = static_cast<float>(std::pow(10.0, -3.0 * seconds / c.decaySeconds));
    const double highRatio = std::pow(10.0, -3.0 * seconds / c.decaySeconds);
    line.dampingPole = static_cast<float>((1.0 - highRatio) / (1.0 + highRatio));
    line.injection = std::sqrt(1.0f - line.feedback * line.feedback) * kInvSqrt8;
  }
  protectionRelease_ = std::exp(-1.0f / (fs * .080f));
  reset();
}

void RadioRoomRenderer::reset() {
  writeIndex_ = 0;
  protectionGain_ = 1.0f;
  std::fill(history_.begin(), history_.end(), 0.0f);
  for (auto& ear : paths_)
    for (auto& path : ear) {
      path.wall.reset();
      path.head.reset();
    }
  for (auto& line : late_) {
    std::fill(line.buffer.begin(), line.buffer.end(), 0.0f);
    line.index = 0;
    line.dampingState = 0.0f;
  }
}

RadioRoomSample RadioRoomRenderer::process(float mono) {
  if (!enabled_)
    return {mono, mono};
  history_[writeIndex_] = mono;
  std::array<float, 2> out{};
  for (size_t ear = 0; ear < 2; ++ear) {
    for (auto& path : paths_[ear]) {
      float ray = 0.0f;
      for (const auto& tap : path.taps) {
        const size_t index = writeIndex_ >= tap.delay
                                 ? writeIndex_ - tap.delay
                                 : writeIndex_ + history_.size() - tap.delay;
        ray += tap.weight * history_[index];
      }
      out[ear] += path.gain * path.head.process(path.wall.process(ray));
    }
  }
  if (++writeIndex_ == history_.size())
    writeIndex_ = 0;
  std::array<float, 8> feedback{};
  float sum = 0.0f, tailLeft = 0.0f, tailRight = 0.0f;
  for (size_t i = 0; i < late_.size(); ++i) {
    auto& line = late_[i];
    const float delayed = line.buffer[line.index];
    line.dampingState =
        (1.0f - line.dampingPole) * delayed + line.dampingPole * line.dampingState;
    feedback[i] = line.feedback * line.dampingState;
    sum += feedback[i];
    tailLeft += (i & 1 ? -1.0f : 1.0f) * delayed * kInvSqrt8;
    tailRight += (i & 2 ? -1.0f : 1.0f) * delayed * kInvSqrt8;
  }
  for (size_t i = 0; i < late_.size(); ++i) {
    auto& line = late_[i];
    line.buffer[line.index] = feedback[i] - .25f * sum + line.injection * mono;
    if (++line.index == line.buffer.size())
      line.index = 0;
  }
  out[0] = outputGain_ * (out[0] + lateLevel_ * tailLeft);
  out[1] = outputGain_ * (out[1] + lateLevel_ * (headphones_ ? tailRight : tailLeft));
  // Summing acoustic paths can exceed the receiver's digital ceiling. Linked
  // peak protection preserves interaural level differences without extra delay.
  const float peak = std::max(std::fabs(out[0]), std::fabs(out[1]));
  const float required = peak > .94f ? .94f / peak : 1.0f;
  protectionGain_ = std::min(
      required, protectionRelease_ * protectionGain_ + (1.0f - protectionRelease_));
  return {out[0] * protectionGain_, out[1] * protectionGain_};
}
