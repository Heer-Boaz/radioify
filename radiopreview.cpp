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
  float lowMax = sampleRate * 0.40f;
  float highMax = sampleRate * safeMaxFraction;
  ifLowHz =
      clampfLocal(std::max(ifStrip.ifLowBaseHz, 120.0f), 40.0f, lowMax);
  float ifSpan =
      clampfLocal(safeBw, 400.0f, std::max(400.0f, highMax - ifLowHz));
  ifHighHz = clampfLocal(ifLowHz + ifSpan, ifLowHz + 400.0f, highMax);
  carrierHz = 0.5f * (ifLowHz + ifHighHz);
  float lowerSidebandRoom = std::max(80.0f, carrierHz - ifLowHz);
  float upperSidebandRoom = std::max(80.0f, ifHighHz - carrierHz);
  float sidebandRoom = std::min(lowerSidebandRoom, upperSidebandRoom);
  audioBandwidthHz = clampfLocal(0.82f * sidebandRoom, 180.0f,
                                 std::min(4600.0f, sampleRate * 0.18f));

  float stageGainRef = std::max(ifStrip.stageGain, 1e-3f);
  carrierAmplitude = clampfLocal(
      0.78f * radio.demod.am.controlVoltageRef / stageGainRef, 0.06f, 0.22f);
  modulationIndex = 0.82f;
  modulationLimit = 0.92f;
  modulationRef = 0.30f;
  programLevelAtk = std::exp(-1.0f / (sampleRate * 0.0025f));
  programLevelRel = std::exp(-1.0f / (sampleRate * 0.160f));

  programHp.setHighpass(sampleRate, 45.0f, kRadioBiquadQ);
  programLp1.setLowpass(sampleRate, audioBandwidthHz, kRadioBiquadQ);
  programLp2.setLowpass(sampleRate, audioBandwidthHz, kRadioBiquadQ);
  reset();
}

void PcmToIfPreviewModulator::reset() {
  phase = 0.0f;
  programLevelEnv = 0.0f;
  programHp.reset();
  programLp1.reset();
  programLp2.reset();
}

void PcmToIfPreviewModulator::processBlock(Radio1938& radio,
                                           float* samples,
                                           uint32_t frames,
                                           uint32_t channels,
                                           float makeupGain) {
  if (!samples || frames == 0 || channels == 0) return;

  auto nextIfSample = [&](float x) {
    float program = programHp.process(x);
    program = programLp1.process(program);
    program = programLp2.process(program);
    float a = std::fabs(program);
    if (a > programLevelEnv) {
      programLevelEnv =
          programLevelAtk * programLevelEnv + (1.0f - programLevelAtk) * a;
    } else {
      programLevelEnv =
          programLevelRel * programLevelEnv + (1.0f - programLevelRel) * a;
    }

    float levelRef = std::max(modulationRef, programLevelEnv);
    float mod = modulationIndex * (program / levelRef);
    mod = clampfLocal(mod, -modulationLimit, modulationLimit);
    float envelope = carrierAmplitude * (1.0f + mod);
    float y = envelope * std::sin(phase);
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
  float foldGain = 1.0f / static_cast<float>(channels);
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
