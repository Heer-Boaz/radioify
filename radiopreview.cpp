#include "radiopreview.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr uint32_t kPreviewWarmupFrames = 16384u;

float computeOpenCircuitCarrierRmsVolts(float fieldStrengthVoltsPerMeter,
                                        float antennaEffectiveHeightMeters) {
  return std::max(fieldStrengthVoltsPerMeter, 0.0f) *
         std::max(antennaEffectiveHeightMeters, 0.0f);
}

}  // namespace

void PcmToIfPreviewModulator::init(const Radio1938& radio,
                                   float newSampleRate,
                                   float bwHz) {
  sampleRate = newSampleRate;
  audioBandwidthHz = 0.48f * bwHz;
  carrierHz = radio.ifStrip.sourceCarrierHz;
  carrierPhase = 0.0f;
  // Preview mode should resemble a strong local broadcast station rather than
  // a distant fringe signal. With roughly 10 mV/m into a 12 m effective
  // antenna height the explicit RF/IF/audio chain lands near the same carrier
  // order as the standalone AM sanity checks, without returning to the old
  // unrealistic near-volt antenna overdrive.
  fieldStrengthVoltsPerMeter = 0.010f;
  antennaEffectiveHeightMeters = 12.0f;
  modulationIndex = 0.85f;

  programHp.setHighpass(sampleRate, 45.0f, kRadioBiquadQ);
  programLp1.setLowpass(sampleRate, audioBandwidthHz, kRadioBiquadQ);
  programLp2.setLowpass(sampleRate, audioBandwidthHz, kRadioBiquadQ);
  reset();
}

void PcmToIfPreviewModulator::reset() {
  carrierPhase = 0.0f;
  warmedUp = false;
  programHp.reset();
  programLp1.reset();
  programLp2.reset();
}

void PcmToIfPreviewModulator::processBlock(Radio1938& radio,
                                           float* samples,
                                           uint32_t frames,
                                           uint32_t channels) {
  if (!samples || frames == 0 || channels == 0) return;

  auto nextProgramSample = [&](float x) {
    float program = programHp.process(x);
    program = programLp1.process(program);
    program = programLp2.process(program);
    return program;
  };
  float carrierStep = kRadioTwoPi * (carrierHz / sampleRate);
  float carrierRms = computeOpenCircuitCarrierRmsVolts(
      fieldStrengthVoltsPerMeter, antennaEffectiveHeightMeters);
  float carrierPeak = std::sqrt(2.0f) * carrierRms;

  if (!warmedUp) {
    monoScratch.assign(std::min<uint32_t>(kPreviewWarmupFrames, 4096u), 0.0f);
    uint32_t remaining = kPreviewWarmupFrames;
    while (remaining > 0u) {
      uint32_t batch =
          std::min<uint32_t>(remaining, static_cast<uint32_t>(monoScratch.size()));
      for (uint32_t frame = 0; frame < batch; ++frame) {
        monoScratch[frame] = carrierPeak * std::cos(carrierPhase);
        carrierPhase += carrierStep;
        if (carrierPhase >= kRadioTwoPi) carrierPhase -= kRadioTwoPi;
      }
      radio.processIfReal(monoScratch.data(), batch);
      remaining -= batch;
    }
    // Do not carry a fully charged AVC/AFC state from the carrier-only warmup
    // into the first audible preview block.
    radio.demod.am.avcRect = 0.0f;
    radio.demod.am.avcEnv = 0.0f;
    radio.demod.am.afcError = 0.0f;
    radio.controlSense.reset();
    radio.controlBus.reset();
    radio.tuning.afcCorrectionHz = 0.0f;
    radio.diagnostics.reset();
    warmedUp = true;
  }

  monoScratch.resize(frames);

  if (channels == 1u) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
      float program = nextProgramSample(samples[frame]);
      float envelopeFactor = std::max(0.0f, 1.0f + modulationIndex * program);
      float envelope = carrierPeak * envelopeFactor;
      monoScratch[frame] = envelope * std::cos(carrierPhase);
      carrierPhase += carrierStep;
      if (carrierPhase >= kRadioTwoPi) carrierPhase -= kRadioTwoPi;
    }
    radio.processIfReal(monoScratch.data(), frames);
    for (uint32_t frame = 0; frame < frames; ++frame) {
      samples[frame] = monoScratch[frame];
    }
    return;
  }

  float foldGain = 1.0f / static_cast<float>(channels);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    float sum = 0.0f;
    size_t base = static_cast<size_t>(frame) * channels;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      sum += samples[base + ch];
    }
    float program = nextProgramSample(sum * foldGain);
    float envelopeFactor = std::max(0.0f, 1.0f + modulationIndex * program);
    float envelope = carrierPeak * envelopeFactor;
    monoScratch[frame] = envelope * std::cos(carrierPhase);
    carrierPhase += carrierStep;
    if (carrierPhase >= kRadioTwoPi) carrierPhase -= kRadioTwoPi;
  }

  radio.processIfReal(monoScratch.data(), frames);

  for (uint32_t frame = 0; frame < frames; ++frame) {
    float y = monoScratch[frame];
    size_t base = static_cast<size_t>(frame) * channels;
    for (uint32_t ch = 0; ch < channels; ++ch) {
      samples[base + ch] = y;
    }
  }
}
