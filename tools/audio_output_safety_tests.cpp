#include "audio/output_volume_safety.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

void fail(const char* message) {
  std::cerr << message << "\n";
  std::exit(1);
}

void expectNear(float actual, float expected, float tolerance, const char* what) {
  if (std::fabs(actual - expected) > tolerance) {
    std::cerr << what << " expected=" << expected << " actual=" << actual
              << "\n";
    std::exit(1);
  }
}

void expectTransparentBelowCeiling() {
  OutputVolumeSafetyState state;
  float samples[] = {0.10f, -0.25f, 0.40f, -0.30f};
  bool limited = applyOutputVolumeSafety(samples, 2, 2, 1.5f, 48000, state);
  if (limited) fail("Output safety limited a below-ceiling block.");
  expectNear(state.gain, 1.0f, 1e-6f, "transparent gain");
  expectNear(samples[0], 0.15f, 1e-6f, "sample 0");
  expectNear(samples[1], -0.375f, 1e-6f, "sample 1");
  expectNear(samples[2], 0.60f, 1e-6f, "sample 2");
  expectNear(samples[3], -0.45f, 1e-6f, "sample 3");
}

void expectLinearAttenuationAboveCeiling() {
  OutputVolumeSafetyState state;
  float samples[] = {0.25f, 0.50f, -0.90f, 0.40f};
  bool limited = applyOutputVolumeSafety(samples, 2, 2, 2.0f, 48000, state);
  if (!limited) fail("Output safety failed to report limiting.");
  float expectedGain = 0.98f / 1.80f;
  expectNear(state.gain, expectedGain, 1e-6f, "limited gain");
  for (float sample : samples) {
    if (std::fabs(sample) > 0.98001f) {
      fail("Output safety exceeded the analog ceiling.");
    }
  }
  expectNear(samples[0] / 0.25f, 2.0f * expectedGain, 1e-6f,
             "linear ratio 0");
  expectNear(samples[1] / 0.50f, 2.0f * expectedGain, 1e-6f,
             "linear ratio 1");
  expectNear(samples[2] / -0.90f, 2.0f * expectedGain, 1e-6f,
             "linear ratio 2");
}

void expectSlowReleaseAfterLimiting() {
  OutputVolumeSafetyState state;
  float hot[] = {1.0f, -1.0f};
  applyOutputVolumeSafety(hot, 1, 2, 2.0f, 48000, state);
  float limitedGain = state.gain;

  float quiet[] = {0.10f, -0.10f};
  bool limited = applyOutputVolumeSafety(quiet, 1, 2, 1.0f, 48000, state);
  if (!limited) fail("Output safety did not hold release state.");
  if (!(state.gain > limitedGain && state.gain < 1.0f)) {
    fail("Output safety release did not move gradually toward unity.");
  }
}

void expectDeclickRampIsLinearPerFrame() {
  OutputVolumeSafetyState state;
  primeOutputVolumeSafetyRamp(state, 4);

  float samples[] = {
      1.0f, 0.5f,
      1.0f, 0.5f,
      1.0f, 0.5f,
      1.0f, 0.5f,
  };
  bool limited = applyOutputVolumeSafety(samples, 4, 2, 0.5f, 48000, state);
  if (limited) fail("Declick ramp reported clipping.");
  expectNear(samples[0], 0.125f, 1e-6f, "ramp frame 0 left");
  expectNear(samples[1], 0.0625f, 1e-6f, "ramp frame 0 right");
  expectNear(samples[2], 0.250f, 1e-6f, "ramp frame 1 left");
  expectNear(samples[3], 0.125f, 1e-6f, "ramp frame 1 right");
  expectNear(samples[4], 0.375f, 1e-6f, "ramp frame 2 left");
  expectNear(samples[5], 0.1875f, 1e-6f, "ramp frame 2 right");
  expectNear(samples[6], 0.500f, 1e-6f, "ramp frame 3 left");
  expectNear(samples[7], 0.250f, 1e-6f, "ramp frame 3 right");
  if (state.rampFramesRemaining != 0 || state.rampFramesTotal != 0) {
    fail("Declick ramp did not finish cleanly.");
  }
}

void expectDefaultDeclickRampFollowsSampleRate() {
  if (outputVolumeSafetyDefaultRampFrames(48000) != 480) {
    fail("Default output ramp is not 10 ms at 48 kHz.");
  }
  if (outputVolumeSafetyDefaultRampFrames(0) != 1) {
    fail("Default output ramp did not handle zero sample rate.");
  }
}

void expectTailFadeOnlyTouchesAudioTail() {
  float samples[] = {
      1.0f, 0.50f,
      0.8f, 0.40f,
      0.6f, 0.30f,
      0.4f, 0.20f,
      0.0f, 0.00f,
  };
  fadeOutputTailToSilence(samples, 4, 2, 2);
  expectNear(samples[0], 1.0f, 1e-6f, "tail fade frame 0 left");
  expectNear(samples[1], 0.5f, 1e-6f, "tail fade frame 0 right");
  expectNear(samples[2], 0.8f, 1e-6f, "tail fade frame 1 left");
  expectNear(samples[3], 0.4f, 1e-6f, "tail fade frame 1 right");
  expectNear(samples[4], 0.3f, 1e-6f, "tail fade frame 2 left");
  expectNear(samples[5], 0.15f, 1e-6f, "tail fade frame 2 right");
  expectNear(samples[6], 0.0f, 1e-6f, "tail fade frame 3 left");
  expectNear(samples[7], 0.0f, 1e-6f, "tail fade frame 3 right");
  expectNear(samples[8], 0.0f, 1e-6f, "tail fade zero frame left");
  expectNear(samples[9], 0.0f, 1e-6f, "tail fade zero frame right");
}

void expectRampBoundsStartStep() {
  constexpr uint32_t kSampleRate = 48000;
  const uint32_t rampFrames = outputVolumeSafetyDefaultRampFrames(kSampleRate);
  OutputVolumeSafetyState state;
  primeOutputVolumeSafetyRamp(state, rampFrames);
  std::vector<float> samples(static_cast<size_t>(rampFrames) + 16, 0.5f);
  bool limited = applyOutputVolumeSafety(
      samples.data(), static_cast<uint32_t>(samples.size()), 1, 1.0f,
      kSampleRate, state);
  if (limited) fail("Start ramp reported clipping.");

  float previous = 0.0f;
  float maxStep = 0.0f;
  for (float sample : samples) {
    maxStep = std::max(maxStep, std::fabs(sample - previous));
    previous = sample;
  }
  const float expectedStep = 0.5f / static_cast<float>(rampFrames);
  if (maxStep > expectedStep + 1e-5f) {
    fail("Start ramp did not bound the output step.");
  }
}

void expectTailFadeBoundsStopStep() {
  constexpr uint32_t kSampleRate = 48000;
  const uint32_t rampFrames = outputVolumeSafetyDefaultRampFrames(kSampleRate);
  std::vector<float> samples(static_cast<size_t>(rampFrames) + 16, 0.5f);
  fadeOutputTailToSilence(samples.data(), static_cast<uint32_t>(samples.size()),
                          1, rampFrames);

  float previous = 0.5f;
  float maxStep = 0.0f;
  for (float sample : samples) {
    maxStep = std::max(maxStep, std::fabs(sample - previous));
    previous = sample;
  }
  maxStep = std::max(maxStep, std::fabs(previous));
  const float expectedStep = 0.5f / static_cast<float>(rampFrames);
  if (maxStep > expectedStep + 1e-5f) {
    fail("Tail fade did not bound the stop step.");
  }
}

}  // namespace

int main() {
  expectTransparentBelowCeiling();
  expectLinearAttenuationAboveCeiling();
  expectSlowReleaseAfterLimiting();
  expectDeclickRampIsLinearPerFrame();
  expectDefaultDeclickRampFollowsSampleRate();
  expectTailFadeOnlyTouchesAudioTail();
  expectRampBoundsStartStep();
  expectTailFadeBoundsStopStep();
  return 0;
}
