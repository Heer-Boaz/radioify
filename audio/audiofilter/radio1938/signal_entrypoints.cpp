#include "../../radio.h"
#include "../math/signal_math.h"
#include "audio_pipeline_execution.h"
#include "radio_am_ingress.h"
#include "radio_buffer_io.h"

#include <algorithm>
#include <cmath>

void Radio1938::processIfReal(float* samples, uint32_t frames) {
  if (!samples || frames == 0 || graph.bypass) return;

  RadioBlockControl block{};
  beginRadioPipelineBlock(*this, frames, block);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    RadioSampleContext ctx{};
    ctx.block = &block;
    float x = sampleInterleavedToMono(samples, frame, channels);
    ctx.signal.setRealRf(x);
    float y = runRadioPipelineSample(*this, x, ctx, PassId::Input);
    writeMonoToInterleaved(samples, frame, channels, y);
  }
  finishRadioPipelineBlock(*this);
}

void Radio1938::processAmAudio(const float* audioSamples,
                               float* outSamples,
                               uint32_t frames,
                               float receivedCarrierRmsVolts,
                               float modulationIndex,
                               const RadioAmReceptionSample* receptionSamples) {
  if (!audioSamples || !outSamples || frames == 0 || graph.bypass) return;

  auto& rfScratch = iqInput.rfScratch;
  if (rfScratch.size() < frames) rfScratch.resize(frames);
  float* rf = rfScratch.data();
  const float safeSampleRate = std::max(sampleRate, 1.0f);
  const float carrierHz = resolvedInputCarrierHz();
  const float carrierStep = kRadioTwoPi * (carrierHz / safeSampleRate);
  const float carrierStepCos = std::cos(carrierStep);
  const float carrierStepSin = std::sin(carrierStep);
  const float carrierPeak =
      std::sqrt(2.0f) * std::max(receivedCarrierRmsVolts, 0.0f);
  float phase = iqInput.iqPhase;
  float carrierCos = std::cos(phase);
  float carrierSin = std::sin(phase);

  for (uint32_t frame = 0; frame < frames; ++frame) {
    const float sampleVal = audioSamples[frame];
    const float envelopeFactor =
        std::max(0.0f, 1.0f + modulationIndex * sampleVal);
    const float envelope = carrierPeak * envelopeFactor;
    if (receptionSamples) {
      const auto& reception = receptionSamples[frame];
      rf[frame] =
          envelope * (reception.desiredCarrierI * carrierCos -
                      reception.desiredCarrierQ * carrierSin) +
          reception.additiveRf;
    } else {
      rf[frame] = envelope * carrierCos;
    }
    advanceNormalizedOscillator(carrierStepCos, carrierStepSin, carrierCos,
                                carrierSin);
    phase += carrierStep;
    if (phase >= kRadioTwoPi) phase -= kRadioTwoPi;
  }

  iqInput.iqPhase = phase;
  RadioBlockControl block{};
  beginRadioPipelineBlock(*this, frames, block);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    RadioSampleContext ctx{};
    ctx.block = &block;
    float x = rf[frame];
    ctx.signal.setRealRf(x);
    float y = runRadioPipelineSample(*this, x, ctx, PassId::Input);
    writeMonoToInterleaved(outSamples, frame, channels, y);
  }
  finishRadioPipelineBlock(*this);
}

void Radio1938::processIqBaseband(const float* iqInterleaved,
                                  float* outSamples,
                                  uint32_t frames) {
  if (!iqInterleaved || !outSamples || frames == 0 || graph.bypass) return;

  RadioBlockControl block{};
  beginRadioPipelineBlock(*this, frames, block);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    RadioSampleContext ctx{};
    ctx.block = &block;
    const size_t base = static_cast<size_t>(frame) * 2u;
    const float i = iqInterleaved[base];
    const float q = iqInterleaved[base + 1u];
    ctx.signal.setComplexEnvelope(i, q);
    float y = runRadioPipelineSample(*this, i, ctx, PassId::Mixer);
    writeMonoToInterleaved(outSamples, frame, channels, y);
  }
  finishRadioPipelineBlock(*this);
}
