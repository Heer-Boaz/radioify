#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>

namespace {

void ensureAMDetectorConfigured(Radio1938& radio) {
  auto& demod = radio.demod;
  const auto& ifStrip = radio.ifStrip;
  const auto& tuning = radio.tuning;
  uint32_t desiredRevision =
      ifStrip.enabled ? ifStrip.appliedConfigRevision : tuning.configRevision;
  if (demod.appliedConfigRevision == desiredRevision) return;

  if (ifStrip.enabled) {
    demod.am.setSenseWindow(ifStrip.senseLowHz, ifStrip.senseHighHz);
    demod.am.setBandwidth(ifStrip.demodBandwidthHz, ifStrip.demodTuneOffsetHz);
  } else {
    float senseHighHz = std::max(180.0f, 2.0f * tuning.tunedBw);
    demod.am.setSenseWindow(0.0f, senseHighHz);
    demod.am.setBandwidth(tuning.tunedBw, tuning.tuneAppliedHz);
  }
  demod.appliedConfigRevision = desiredRevision;
}

}  // namespace

void RadioAMDetectorNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& demod = radio.demod;
  demod.appliedConfigRevision = 0;
  demod.am.init(radio.sampleRate, initCtx.tunedBw, radio.tuning.tuneOffsetHz);
  ensureAMDetectorConfigured(radio);
}

void RadioAMDetectorNode::reset(Radio1938& radio) {
  radio.demod.am.reset();
  radio.demod.appliedConfigRevision = 0;
}

float RadioAMDetectorNode::run(Radio1938& radio,
                               float y,
                               RadioSampleContext& ctx) {
  auto& demod = radio.demod;
  ensureAMDetectorConfigured(radio);
  if (radio.ifStrip.enabled) {
    y = demod.am.run(ctx.signal.detectorInputI, ctx.signal.detectorInputQ,
                     ctx.derived.demodIfNoiseAmp, radio,
                     ctx.derived.demodIfCrackleAmp,
                     ctx.derived.demodIfCrackleRate);
  } else if (ctx.signal.mode == SourceInputMode::ComplexEnvelope) {
    y = demod.am.run(ctx.signal.i, ctx.signal.q, ctx.derived.demodIfNoiseAmp,
                     radio, ctx.derived.demodIfCrackleAmp,
                     ctx.derived.demodIfCrackleRate);
  } else {
    y = demod.am.run(y, 0.0f, ctx.derived.demodIfNoiseAmp, radio,
                     ctx.derived.demodIfCrackleAmp,
                     ctx.derived.demodIfCrackleRate);
  }
  radio.controlSense.controlVoltageSense =
      clampf(demod.am.avcEnv / std::max(demod.am.controlVoltageRef, 1e-6f),
             0.0f, 1.25f);
  radio.controlSense.tuningErrorSense = demod.am.afcError;
  return y;
}
