#include "../../radio.h"
#include "audio_pipeline_execution.h"
#include "radio_buffer_io.h"

#include <algorithm>
#include <cmath>
#include <vector>

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
                               float modulationIndex) {
  if (!audioSamples || !outSamples || frames == 0 || graph.bypass) return;

  std::vector<float> rfScratch(frames);
  const float safeSampleRate = std::max(sampleRate, 1.0f);
  const float carrierHz = resolvedInputCarrierHz();
  const float carrierStep = kRadioTwoPi * (carrierHz / safeSampleRate);
  const float carrierPeak =
      std::sqrt(2.0f) * std::max(receivedCarrierRmsVolts, 0.0f);
  float phase = iqInput.iqPhase;

  // Ensure input drive is within [-1.0, 1.0] but do not up-scale low-level
  // caller audio; the caller is responsible for explicitly providing a
  // normalized program (peak 1.0) when full modulationIndex should apply.
  float maxAbs = 0.0f;
  for (uint32_t i = 0; i < frames; ++i) {
    maxAbs = std::max(maxAbs, std::fabs(audioSamples[i]));
  }
  float normScale = 1.0f;
  if (maxAbs > 1.0f) {
    normScale = 1.0f / maxAbs;
  }

  for (uint32_t frame = 0; frame < frames; ++frame) {
    // Normalize audioSamples peak to 1.0 if it's lower so modulationIndex maps
    // directly to effective modulation depth. This prevents callers accidentally
    // passing a low-amplitude program (e.g. 0.35 peak) from reducing the
    // intended modulation.
    const float sampleVal = audioSamples[frame] * normScale;
    const float envelopeFactor =
        std::max(0.0f, 1.0f + modulationIndex * sampleVal);
    const float envelope = carrierPeak * envelopeFactor;
    rfScratch[frame] = envelope * std::cos(phase);
    phase += carrierStep;
    if (phase >= kRadioTwoPi) phase -= kRadioTwoPi;
  }

  iqInput.iqPhase = phase;
  RadioBlockControl block{};
  beginRadioPipelineBlock(*this, frames, block);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    RadioSampleContext ctx{};
    ctx.block = &block;
    float x = rfScratch[frame];
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
