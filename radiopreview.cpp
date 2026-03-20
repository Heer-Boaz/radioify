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
  float safeBw = std::max(bwHz, radio.ifStrip.ifMinBwHz);
  audioBandwidthHz = clampfLocal(0.48f * safeBw, 180.0f,
                                 std::min(4600.0f, sampleRate * 0.45f));
  carrierAmplitude = 1.0f;
  modulationIndex = 0.85f;
  modulationLimit = 0.98f;

  programHp.setHighpass(sampleRate, 45.0f, kRadioBiquadQ);
  programLp1.setLowpass(sampleRate, audioBandwidthHz, kRadioBiquadQ);
  programLp2.setLowpass(sampleRate, audioBandwidthHz, kRadioBiquadQ);
  reset();
}

void PcmToIfPreviewModulator::reset() {
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

  auto nextProgramSample = [&](float x) {
    float program = programHp.process(x);
    program = programLp1.process(program);
    program = programLp2.process(program);
    return program;
  };

  monoScratch.resize(frames);

  if (channels == 1u) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
      monoScratch[frame] = nextProgramSample(samples[frame]);
    }
    radio.processAmAudio(monoScratch.data(), monoScratch.data(), frames,
                         carrierAmplitude, modulationIndex, modulationLimit);
    applyMonoGain(monoScratch.data(), frames, makeupGain);
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
    monoScratch[frame] = nextProgramSample(sum * foldGain);
  }

  radio.processAmAudio(monoScratch.data(), monoScratch.data(), frames,
                       carrierAmplitude, modulationIndex, modulationLimit);
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
