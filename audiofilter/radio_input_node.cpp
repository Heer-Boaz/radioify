#include "../radio.h"

#include <algorithm>
#include <cmath>

void RadioInputNode::init(Radio1938& radio, RadioInitContext&) {
  auto& input = radio.input;
  input.autoEnvAtk =
      std::exp(-1.0f / (radio.sampleRate * (input.autoEnvAttackMs / 1000.0f)));
  input.autoEnvRel =
      std::exp(-1.0f / (radio.sampleRate * (input.autoEnvReleaseMs / 1000.0f)));
  input.autoGainAtk =
      std::exp(-1.0f / (radio.sampleRate * (input.autoGainAttackMs / 1000.0f)));
  input.autoGainRel =
      std::exp(-1.0f / (radio.sampleRate * (input.autoGainReleaseMs / 1000.0f)));
  float sourceR = input.sourceResistanceOhms;
  float loadR = input.inputResistanceOhms;
  input.sourceDivider = loadR / (sourceR + loadR);
  if (input.couplingCapFarads > 0.0f) {
    float hpHz = 1.0f / (kRadioTwoPi * (sourceR + loadR) * input.couplingCapFarads);
    input.sourceCouplingHp.setHighpass(radio.sampleRate, hpHz, kRadioBiquadQ);
  } else {
    input.sourceCouplingHp = Biquad{};
  }
}

void RadioInputNode::reset(Radio1938& radio) {
  radio.input.autoEnv = 0.0f;
  radio.input.autoGainDb = 0.0f;
  radio.input.sourceCouplingHp.reset();
}

float RadioInputNode::process(Radio1938& radio,
                              float x,
                              const RadioSampleContext&) {
  auto& input = radio.input;
  if (radio.sourceFrame.mode == SourceInputMode::RealRf) {
    x *= input.sourceDivider;
    if (input.couplingCapFarads > 0.0f) {
      x = input.sourceCouplingHp.process(x);
    }
  }
  x *= radio.globals.inputPad;
  if (!radio.globals.enableAutoLevel) return x;

  float ax = std::fabs(x);
  if (ax > input.autoEnv) {
    input.autoEnv = input.autoEnvAtk * input.autoEnv + (1.0f - input.autoEnvAtk) * ax;
  } else {
    input.autoEnv = input.autoEnvRel * input.autoEnv + (1.0f - input.autoEnvRel) * ax;
  }
  float envDb = lin2db(input.autoEnv);
  float targetBoostDb =
      std::clamp(radio.globals.autoTargetDb - envDb, 0.0f,
                 radio.globals.autoMaxBoostDb);
  if (targetBoostDb < input.autoGainDb) {
    input.autoGainDb = input.autoGainAtk * input.autoGainDb +
                       (1.0f - input.autoGainAtk) * targetBoostDb;
  } else {
    input.autoGainDb = input.autoGainRel * input.autoGainDb +
                       (1.0f - input.autoGainRel) * targetBoostDb;
  }
  return x * db2lin(input.autoGainDb);
}
