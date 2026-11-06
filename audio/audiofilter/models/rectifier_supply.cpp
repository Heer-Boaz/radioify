#include "rectifier_supply.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {
constexpr double twoPi = 6.2831853071795864769;
}

void RectifierSupply::configure(const Config& config, double sampleRate, double mainsHz) {
  config_ = config;
  dt_ = 1 / sampleRate;
  phaseStep_ = twoPi * mainsHz / sampleRate;
  Matrix matrix{};
  capacitance_ = {};
  const auto stamp = [](Matrix& m, int a, int b, double g) {
    m[a][a] += g;
    if (b >= 0) {
      m[b][b] += g;
      m[a][b] -= g;
      m[b][a] -= g;
    }
  };
  stamp(capacitance_, Reservoir, Return, config.reservoirFarads / dt_);
  stamp(capacitance_, Main, config.filterReturnsToCentreTap ? Return : -1,
        config.filterFarads / dt_);
  stamp(capacitance_, Return, -1, config.returnFarads / dt_);
  stamp(capacitance_, Auxiliary, -1, config.auxiliaryFarads / dt_);
  stamp(capacitance_, Audio, -1, config.audioFarads / dt_);
  matrix = capacitance_;
  filterG_ = 1 / (config.filterResistanceOhms + config.filterHenries / dt_);
  returnG_ = 1 / (config.returnResistanceOhms + config.returnHenries / dt_);
  stamp(matrix, Reservoir, Main, filterG_);
  stamp(matrix, Return, -1, returnG_);
  const double auxiliaryZ =
      config.auxiliaryResistanceOhms + config.auxiliaryHenries / dt_;
  auxiliaryG_ = auxiliaryZ > 0 ? 1 / auxiliaryZ : 0;
  stamp(matrix, Main, Auxiliary, auxiliaryG_);
  if (config.audioResistanceOhms > 0)
    stamp(matrix, Auxiliary, Audio, 1 / config.audioResistanceOhms);
  // Exact wires merge nodes. A tiny artificial resistance makes the small
  // receiver's unfiltered audio rail ill-conditioned and obscures its topology.
  std::array<int, NodeCount> map{0, 1, 2, 3, 4};
  int count = 3;
  map[Auxiliary] = auxiliaryZ > 0 ? count++ : map[Main];
  map[Audio] = config.audioResistanceOhms > 0 ? count++ : map[Auxiliary];
  Matrix reduced{};
  for (int i = 0; i < NodeCount; ++i)
    for (int j = 0; j < NodeCount; ++j) reduced[map[i]][map[j]] += matrix[i][j];
  matrix = reduced;
  inverse_ = {};
  for (int i = 0; i < count; ++i) inverse_[i][i] = 1;
  // Cold-path inversion. The passive conductance matrix is positive definite.
  for (int col = 0; col < count; ++col) {
    const double pivot = matrix[col][col];
    assert(pivot > 0 && std::isfinite(pivot));
    for (int j = 0; j < count; ++j) {
      matrix[col][j] /= pivot;
      inverse_[col][j] /= pivot;
    }
    for (int row = 0; row < count; ++row)
      if (row != col) {
        const double scale = matrix[row][col];
        for (int j = 0; j < count; ++j) {
          matrix[row][j] -= scale * matrix[col][j];
          inverse_[row][j] -= scale * inverse_[col][j];
        }
      }
  }
  const Matrix compactInverse = inverse_;
  for (int i = 0; i < NodeCount; ++i)
    for (int j = 0; j < NodeCount; ++j) inverse_[i][j] = compactInverse[map[i]][map[j]];
  for (int i = 0; i < NodeCount; ++i)
    injection_[i] = inverse_[i][Reservoir] - inverse_[i][Return];
  diodeR_ = config.secondaryResistanceOhms + injection_[Reservoir] - injection_[Return];
}

void RectifierSupply::step(Load load) {
  const auto previous = state_.volts;
  std::array<double, NodeCount> rhs{}, unloaded{};
  for (int i = 0; i < NodeCount; ++i)
    for (int j = 0; j < NodeCount; ++j) rhs[i] += capacitance_[i][j] * state_.volts[j];
  const double filterHistory = filterG_ * config_.filterHenries / dt_ * state_.filterAmps;
  const double returnHistory = returnG_ * config_.returnHenries / dt_ * state_.returnAmps;
  const double auxiliaryHistory =
      auxiliaryG_ * config_.auxiliaryHenries / dt_ * state_.auxiliaryAmps;
  rhs[Reservoir] -= filterHistory;
  rhs[Main] += filterHistory - load.mainAmps - auxiliaryHistory;
  rhs[Return] -= returnHistory;
  rhs[Auxiliary] += auxiliaryHistory - load.auxiliaryAmps;
  rhs[Audio] -= load.audioAmps;
  for (int i = 0; i < NodeCount; ++i)
    for (int j = 0; j < NodeCount; ++j) unloaded[i] += inverse_[i][j] * rhs[j];
  state_.phase = std::fmod(state_.phase + phaseStep_, twoPi);
  // Zero mains frequency explicitly selects a DC bench source; this is useful
  // for isolating signal-path aliasing without discarding load regulation.
  const double input =
      secondaryPeak_ * (phaseStep_ > 0 ? std::abs(std::sin(state_.phase)) : 1);
  rectifierCurrent_ =
      config_.rectifier.current(input - unloaded[Reservoir] + unloaded[Return], diodeR_);
  for (int i = 0; i < NodeCount; ++i)
    state_.volts[i] = unloaded[i] + injection_[i] * rectifierCurrent_;
  state_.filterAmps = filterG_ * (volts(Reservoir) - volts(Main)) + filterHistory;
  state_.returnAmps = returnG_ * volts(Return) + returnHistory;
  state_.audioFeedAmps =
      load.audioAmps + config_.audioFarads / dt_ * (volts(Audio) - previous[Audio]);
  state_.auxiliaryAmps =
      auxiliaryG_ > 0
          ? auxiliaryG_ * (volts(Main) - volts(Auxiliary)) + auxiliaryHistory
          : load.auxiliaryAmps + state_.audioFeedAmps +
                config_.auxiliaryFarads / dt_ * (volts(Auxiliary) - previous[Auxiliary]);
}

void RectifierSupply::start(double nominalMainVolts, Load load) {
  const double total = load.mainAmps + load.auxiliaryAmps + load.audioAmps;
  state_ = {};
  state_.volts[Return] = -total * config_.returnResistanceOhms;
  state_.volts[Main] = nominalMainVolts;
  state_.volts[Reservoir] = nominalMainVolts + total * config_.filterResistanceOhms;
  state_.volts[Auxiliary] = nominalMainVolts - (load.auxiliaryAmps + load.audioAmps) *
                                                   config_.auxiliaryResistanceOhms;
  state_.volts[Audio] = volts(Auxiliary) - load.audioAmps * config_.audioResistanceOhms;
  state_.filterAmps = total;
  state_.returnAmps = -total;
  state_.auxiliaryAmps = load.auxiliaryAmps + load.audioAmps;
  state_.audioFeedAmps = load.audioAmps;
  secondaryPeak_ = volts(Reservoir) - volts(Return) +
                   std::pow(total / config_.rectifier.perveance, 2.0 / 3.0) +
                   total * config_.secondaryResistanceOhms;
  if (phaseStep_ > 0)
    secondaryPeak_ +=
        total / (4 * (phaseStep_ / (twoPi * dt_)) * config_.reservoirFarads);
  // Calibrate the unknown transformer ratio against the receiver's DC voltage
  // prior at idle, once, then freeze it. Runtime load changes are never levelled
  // out. Starting near the DC solution avoids charging a cold radio on playback.
  const int settle = static_cast<int>(std::ceil(.2 / dt_));
  for (int pass = 0; pass < 4; ++pass) {
    double sum = 0;
    for (int n = 0; n < settle; ++n) {
      step(load);
      if (n >= settle / 2) sum += volts(Main);
    }
    const double mean = sum / (settle - settle / 2);
    if (pass != 3) secondaryPeak_ += nominalMainVolts - mean;
  }
  initial_ = state_;
}
