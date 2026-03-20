#include "radiopreview.h"

#include <algorithm>
#include <cmath>

namespace {

float clampfLocal(float v, float lo, float hi) {
  return std::clamp(v, lo, hi);
}

void applyMonoGain(float* samples, uint32_t frames, float gain) {
  if (!samples || frames == 0 || gain == 1.0f) return;
  for (uint32_t i = 0; i < frames; ++i) {
    samples[i] *= gain;
  }
}

}  // namespace

void PcmToIfPreviewModulator::init(const Radio1938& radio,
                                   float newSampleRate,
                                   float bwHz) {
  sampleRate = std::max(newSampleRate, 1.0f);
  const auto& ifStrip = radio.ifStrip;
  float safeMaxFraction = std::max(ifStrip.ifMaxFraction, 0.10f);
  float safeBw =
      clampfLocal(bwHz, std::max(400.0f, ifStrip.ifMinBwHz),
                  sampleRate * safeMaxFraction);
  float ifLow =
      clampfLocal(std::max(ifStrip.ifLowBaseHz, 120.0f), 40.0f, sampleRate * 0.40f);
  float ifHigh =
      clampfLocal(ifLow + safeBw, ifLow + 400.0f, sampleRate * safeMaxFraction);
  float ifSpan = std::max(400.0f, ifHigh - ifLow);
  carrierHz = clampfLocal(0.5f * (ifLow + ifHigh), ifLow + 0.20f * ifSpan,
                          ifHigh - 0.20f * ifSpan);

  float programHpHz = 55.0f;
  float programLpHz =
      clampfLocal(safeBw * 0.42f, 1400.0f, std::min(4200.0f, sampleRate * 0.18f));
  programHp.setHighpass(sampleRate, programHpHz, kRadioBiquadQ);
  programLp.setLowpass(sampleRate, programLpHz, kRadioBiquadQ);
  reset();
}

void PcmToIfPreviewModulator::reset() {
  phase = 0.0f;
  programHp.reset();
  programLp.reset();
}

void PcmToIfPreviewModulator::processBlock(Radio1938& radio,
                                           float* samples,
                                           uint32_t frames,
                                           uint32_t channels,
                                           float makeupGain) {
  if (!samples || frames == 0 || channels == 0) return;

  auto nextIfSample = [&](float x) {
    float program = programHp.process(x);
    program = programLp.process(program);
    float env = std::max(minEnvelope, carrierLevel + modulationDepth * program);
    env = std::min(env, 1.25f);
    float y = env * std::sin(phase);
    phase += kRadioTwoPi * (carrierHz / std::max(sampleRate, 1.0f));
    if (phase >= kRadioTwoPi) {
      phase = std::fmod(phase, kRadioTwoPi);
    }
    return y;
  };

  if (channels == 1u) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
      samples[frame] = nextIfSample(samples[frame]);
    }
    radio.processIfReal(samples, frames);
    applyMonoGain(samples, frames, makeupGain);
    return;
  }

  monoScratch.resize(frames);
  float foldGain = 1.0f / std::sqrt(static_cast<float>(channels));
  for (uint32_t frame = 0; frame < frames; ++frame) {
    float sum = 0.0f;
    size_t base = static_cast<size_t>(frame) * channels;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      sum += samples[base + ch];
    }
    monoScratch[frame] = nextIfSample(sum * foldGain);
  }

  radio.processIfReal(monoScratch.data(), frames);
  applyMonoGain(monoScratch.data(), frames, makeupGain);

  for (uint32_t frame = 0; frame < frames; ++frame) {
    float y = monoScratch[frame];
    size_t base = static_cast<size_t>(frame) * channels;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      samples[base + ch] = y;
    }
  }
}

bool radioPreviewBlockOverloaded(const Radio1938& radio, uint32_t frames) {
  const auto& diag = radio.diagnostics;
  if (diag.outputClip) return true;
  uint32_t overloadSamples =
      diag.powerClipSamples + diag.speakerClipSamples + diag.outputClipSamples;
  uint32_t threshold = std::max<uint32_t>(4u, frames / 256u);
  return overloadSamples >= threshold;
}
