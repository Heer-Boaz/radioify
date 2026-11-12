#include "audio_pipeline_execution.h"

#include "analysis/radio_calibration_runtime.h"
#include "radio_circuit_observation.h"

#include <algorithm>
#include <cmath>

namespace {

void prepareBlockPasses(Radio1938& radio,
                        uint32_t frames,
                        RadioBlockControl& block) {
  radio.graph.forEachOrderedPass(AudioPassDomain::Block, [&](const auto& pass) {
    if (pass.prepare) pass.prepare(radio, block, frames);
  });
}

void runControlPasses(Radio1938& radio, RadioSampleContext& ctx) {
  radio.graph.forEachOrderedPass(AudioPassDomain::SampleControl,
                                 [&](const auto& pass) {
                                   if (pass.runControl) {
                                     pass.runControl(radio, ctx);
                                   }
                                 });
}

size_t resolveFirstProgramIndex(Radio1938& radio,
                                const RadioSampleContext& ctx,
                                PassId firstProgramPass) {
  const RadioBlockControl* block = ctx.block;
  if (block && block->programStartCached &&
      block->cachedProgramStartPass == firstProgramPass) {
    return block->cachedProgramStartIndex;
  }
  const size_t orderIndex =
      radio.graph.findEntryOrderIndex(AudioPassDomain::Program, firstProgramPass);
  if (block) {
    block->cachedProgramStartPass = firstProgramPass;
    block->cachedProgramStartIndex = orderIndex;
    block->programStartCached = true;
  }
  return orderIndex;
}

void observeProgramPass(Radio1938& radio, const RadioSampleContext& ctx,
                        PassId pass, float output) {
  auto& observation = *ctx.observation;
  switch (pass) {
    case PassId::FrontEnd:
      observation.add(RadioCircuitProbe::AntennaEnvelopeVolts, output);
      break;
    case PassId::Mixer: {
      const float envelope = ctx.signal.mode == SourceInputMode::ComplexEnvelope
          ? std::hypot(ctx.signal.i, ctx.signal.q) : std::fabs(output);
      observation.add(RadioCircuitProbe::ConverterEnvelopeAmps,
                      envelope * std::fabs(radio.mixer.conversionGain));
      break;
    }
    case PassId::IFStrip:
      if (radio.ifStrip.enabled)
        observation.add(RadioCircuitProbe::IfEnvelopeVolts, output);
      break;
    case PassId::Demod:
      observation.add(RadioCircuitProbe::DetectorVolts, radio.demod.am.detectorNode);
      break;
    case PassId::ReceiverCircuit:
      if (radio.receiverCircuit.enabled)
        observation.add(RadioCircuitProbe::AudioPlateSwingVolts, output);
      break;
    case PassId::Power:
      observation.add(RadioCircuitProbe::SpeakerTerminalVolts,
                      radio.speakerStage.physicalDriveVolts);
      observation.add(RadioCircuitProbe::SpeakerCoilAmps,
                      radio.speakerStage.speaker.electricalCurrentAmps);
      observation.add(RadioCircuitProbe::SupplyVolts,
          static_cast<float>(radio.powerSupply.circuit.volts(RectifierSupply::Main)));
      break;
    default:
      break;
  }
}

template <bool Observe>
float runProgramPasses(Radio1938& radio,
                       float y,
                       RadioSampleContext& ctx,
                       size_t firstProgramIndex) {
  const auto& order = radio.graph.order(AudioPassDomain::Program);
  const size_t begin = std::min(firstProgramIndex, order.size());
  const bool calibrationEnabled = radio.calibration.enabled;
  const bool clickTraceEnabled =
      calibrationEnabled && radio.calibration.clickTrace.enabled;
  for (size_t orderIndex = begin; orderIndex < order.size(); ++orderIndex) {
    const auto& pass = radio.graph.pass(order[orderIndex]);
    if (!pass.runSample) continue;
    const float in = y;
    y = pass.runSample(radio, y, ctx);
    if constexpr (Observe) observeProgramPass(radio, ctx, pass.id, y);
    if (clickTraceEnabled) {
      recordClickTracePassOutput(radio, pass.id, y);
    }
    if (calibrationEnabled && !pass.stateOnly) {
      updatePassCalibration(radio, pass.id, in, y);
    }
  }
  return y;
}

}  // namespace

void beginRadioPipelineBlock(Radio1938& radio,
                             uint32_t frames,
                             RadioBlockControl& block) {
  radio.graph.compile();
  radio.diagnostics.reset();
  block = {};
  prepareBlockPasses(radio, frames, block);
}

float runRadioPipelineSample(Radio1938& radio,
                             float x,
                             RadioSampleContext& ctx,
                             PassId firstProgramPass) {
  if (radio.calibration.enabled && radio.calibration.clickTrace.enabled) {
    beginClickTraceSample(radio);
  }
  runControlPasses(radio, ctx);
  const size_t firstProgramIndex =
      resolveFirstProgramIndex(radio, ctx, firstProgramPass);
  const float y = ctx.observation
      ? runProgramPasses<true>(radio, x, ctx, firstProgramIndex)
      : runProgramPasses<false>(radio, x, ctx, firstProgramIndex);
  if (radio.calibration.enabled) {
    radio.calibration.maxDigitalOutput =
        std::max(radio.calibration.maxDigitalOutput, std::fabs(y));
  }
  if (radio.calibration.enabled && radio.calibration.clickTrace.enabled) {
    finishClickTraceSample(radio, y);
  }
  return y;
}

void finishRadioPipelineBlock(Radio1938& radio) {
  if (radio.calibration.enabled) {
    updateCalibrationSnapshot(radio);
  }
}
