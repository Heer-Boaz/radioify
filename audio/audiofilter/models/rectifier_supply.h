#ifndef RADIOIFY_AUDIOFILTER_MODELS_RECTIFIER_SUPPLY_H
#define RADIOIFY_AUDIOFILTER_MODELS_RECTIFIER_SUPPLY_H

#include "thermionic_diode.h"

#include <array>

// Full-wave, centre-tapped supply. Voltages are relative to chassis, including
// the negative transformer centre tap. Fixed-size nodal companions retain
// capacitor charge and choke current; no programme envelope or gain modulation.
class RectifierSupply {
 public:
  enum Node { Reservoir, Main, Return, Auxiliary, Audio, NodeCount };
  struct Config {
    ThermionicDiode rectifier{0.00075, 0.073, 0};
    float secondaryResistanceOhms = 100;
    float reservoirFarads = 16e-6;
    float filterFarads = 8e-6;
    bool filterReturnsToCentreTap = true;
    float filterHenries = 4.75;
    float filterResistanceOhms = 80;
    float returnFarads = 10e-6;
    float returnHenries = 5;
    float returnResistanceOhms = 378;
    float auxiliaryResistanceOhms = 1400;
    float auxiliaryHenries = 8;
    float auxiliaryFarads = 3e-6;
    float audioResistanceOhms = 99000;
    float audioFarads = .05e-6;
  };
  struct State {
    std::array<double, NodeCount> volts{};
    double filterAmps = 0;
    double returnAmps = 0;
    double auxiliaryAmps = 0;
    double audioFeedAmps = 0;
    double phase = 0;
  };
  struct Load {
    double mainAmps = 0;
    double auxiliaryAmps = 0;
    double audioAmps = 0;
  };

  void configure(const Config& config, double sampleRate, double mainsHz);
  void start(double nominalMainVolts, Load idleLoad);
  void reset() { state_ = initial_; }
  void step(Load load);
  const State& state() const { return state_; }
  double volts(Node node) const { return state_.volts[node]; }
  double rectifierCurrent() const { return rectifierCurrent_; }
  double secondaryPeakVolts() const { return secondaryPeak_; }
  void setSecondaryPeakVolts(double peak) { secondaryPeak_ = peak; }
  void setState(const State& state) { state_ = state; }

 private:
  using Matrix = std::array<std::array<double, NodeCount>, NodeCount>;
  Config config_;
  State state_, initial_;
  Matrix inverse_{}, capacitance_{};
  std::array<double, NodeCount> injection_{};
  double dt_ = 0, phaseStep_ = 0;
  double filterG_ = 0, returnG_ = 0, auxiliaryG_ = 0, diodeR_ = 0;
  double secondaryPeak_ = 0, rectifierCurrent_ = 0;
};

#endif
