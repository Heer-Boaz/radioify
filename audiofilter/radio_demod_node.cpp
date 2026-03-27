#include "../radio.h"
#include "math/radio_math.h"

#include <algorithm>

void RadioDemodNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& demod = radio.demod;
  demod.am.init(radio.sampleRate, initCtx.tunedBw,
                radio.tuning.tuneOffsetHz);
}

void RadioDemodNode::reset(Radio1938& radio) { radio.demod.am.reset(); }

float RadioDemodNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext& ctx) {
  auto& demod = radio.demod;
  if (radio.ifStrip.enabled) {
    y = demod.am.processEnvelope(radio.ifStrip.detectorInputI,
                                 radio.ifStrip.detectorInputQ,
                                 ctx.derived.demodIfNoiseAmp,
                                 radio,
                                 ctx.derived.demodIfCrackleAmp,
                                 ctx.derived.demodIfCrackleRate);
  } else if (radio.sourceFrame.mode == SourceInputMode::ComplexEnvelope) {
    y = demod.am.processEnvelope(radio.sourceFrame.i, radio.sourceFrame.q,
                                 ctx.derived.demodIfNoiseAmp,
                                 radio,
                                 ctx.derived.demodIfCrackleAmp,
                                 ctx.derived.demodIfCrackleRate);
  } else {
    y = demod.am.processEnvelope(y, 0.0f, ctx.derived.demodIfNoiseAmp,
                                 radio,
                                 ctx.derived.demodIfCrackleAmp,
                                 ctx.derived.demodIfCrackleRate);
  }
  radio.controlSense.controlVoltageSense =
      clampf(demod.am.avcEnv / std::max(demod.am.controlVoltageRef, 1e-6f),
             0.0f, 1.25f);
  radio.controlSense.tuningErrorSense = demod.am.afcError;
  return y;
}
