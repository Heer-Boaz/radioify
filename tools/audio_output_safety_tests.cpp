#include "audio/output_volume_safety.h"
#include "audio/pipeline_transition.h"
#include "audio/playback_source_buffer.h"
#include "audio/playback_source_priming.h"
#include "audio/radio_bypass_transition.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <iterator>
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
  AudioPipelineTransition transition;
  audioPipelineTransitionRequestOutputFadeIn(transition, 400);

  float samples[] = {
      1.0f, 0.5f,
      1.0f, 0.5f,
      1.0f, 0.5f,
      1.0f, 0.5f,
  };
  audioPipelineTransitionApplyFadeIn(transition, samples, 4, 2);
  expectNear(samples[0], 0.250f, 1e-6f, "ramp frame 0 left");
  expectNear(samples[1], 0.125f, 1e-6f, "ramp frame 0 right");
  expectNear(samples[2], 0.500f, 1e-6f, "ramp frame 1 left");
  expectNear(samples[3], 0.250f, 1e-6f, "ramp frame 1 right");
  expectNear(samples[4], 0.750f, 1e-6f, "ramp frame 2 left");
  expectNear(samples[5], 0.375f, 1e-6f, "ramp frame 2 right");
  expectNear(samples[6], 1.000f, 1e-6f, "ramp frame 3 left");
  expectNear(samples[7], 0.500f, 1e-6f, "ramp frame 3 right");
  if (transition.outputFadeInFramesRemaining != 0 ||
      transition.outputFadeInFramesTotal != 0) {
    fail("Declick ramp did not finish cleanly.");
  }
}

void expectSignalFadeArmsInputAndOutputBoundaries() {
  AudioPipelineTransition transition;
  audioPipelineTransitionRequestSignalFadeIn(transition, 400);

  float input[] = {
      1.0f, 1.0f,
      1.0f, 1.0f,
      1.0f, 1.0f,
      1.0f, 1.0f,
  };
  audioPipelineTransitionApplyInputFadeIn(transition, input, 4, 2);
  expectNear(input[0], 0.250f, 1e-6f, "input ramp frame 0 left");
  expectNear(input[2], 0.500f, 1e-6f, "input ramp frame 1 left");
  expectNear(input[4], 0.750f, 1e-6f, "input ramp frame 2 left");
  expectNear(input[6], 1.000f, 1e-6f, "input ramp frame 3 left");

  float output[] = {
      1.0f, 1.0f,
      1.0f, 1.0f,
      1.0f, 1.0f,
      1.0f, 1.0f,
  };
  audioPipelineTransitionApplyFadeIn(transition, output, 4, 2);
  expectNear(output[0], 0.250f, 1e-6f, "output ramp frame 0 left");
  expectNear(output[2], 0.500f, 1e-6f, "output ramp frame 1 left");
  expectNear(output[4], 0.750f, 1e-6f, "output ramp frame 2 left");
  expectNear(output[6], 1.000f, 1e-6f, "output ramp frame 3 left");
}

void expectDefaultDeclickRampFollowsSampleRate() {
  if (audioPipelineTransitionFrames(48000) != 480) {
    fail("Default pipeline transition ramp is not 10 ms at 48 kHz.");
  }
  if (audioPipelineTransitionFrames(0) != 1) {
    fail("Default pipeline transition ramp did not handle zero sample rate.");
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
  audioPipelineTransitionFadeTailToSilence(samples, 4, 2, 200);
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
  const uint32_t rampFrames = audioPipelineTransitionFrames(kSampleRate);
  AudioPipelineTransition transition;
  audioPipelineTransitionRequestOutputFadeIn(transition, kSampleRate);
  std::vector<float> samples(static_cast<size_t>(rampFrames) + 16, 0.5f);
  audioPipelineTransitionApplyFadeIn(
      transition, samples.data(), static_cast<uint32_t>(samples.size()), 1);

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
  const uint32_t rampFrames = audioPipelineTransitionFrames(kSampleRate);
  std::vector<float> samples(static_cast<size_t>(rampFrames) + 16, 0.5f);
  audioPipelineTransitionFadeTailToSilence(
      samples.data(), static_cast<uint32_t>(samples.size()), 1, kSampleRate);

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

void expectDiscontinuityTransitionPhases() {
  AudioPipelineTransition transition;
  audioPipelineTransitionRequestDiscontinuity(transition, 48000);
  if (!audioPipelineTransitionActive(transition)) {
    fail("Discontinuity transition was not active after request.");
  }
  if (!audioPipelineTransitionBeginFadeOut(transition)) {
    fail("Discontinuity transition did not begin fade-out.");
  }
  if (!audioPipelineTransitionWaitingForCommit(transition)) {
    fail("Discontinuity transition was not ready to commit after fade-out.");
  }
  if (!audioPipelineTransitionBeginCommit(transition)) {
    fail("Discontinuity transition did not begin commit.");
  }
  if (!audioPipelineTransitionWaitingForCommit(transition)) {
    fail("Discontinuity transition did not hold output during commit.");
  }
  if (!audioPipelineTransitionCommitInProgress(transition)) {
    fail("Discontinuity transition did not expose commit-in-progress state.");
  }
  if (!audioPipelineTransitionFinishCommit(transition)) {
    fail("Discontinuity transition refused to finish the current request.");
  }
  if (transition.phase.load(std::memory_order_relaxed) !=
      AudioPipelineTransitionPhase::Idle) {
    fail("Discontinuity transition did not return to idle.");
  }
  if (transition.inputFadeInRequestFrames.load(std::memory_order_relaxed) != 0 ||
      transition.outputFadeInRequestFrames.load(std::memory_order_relaxed) !=
          0) {
    fail("Commit completion armed a fade outside the signal owner.");
  }
  audioPipelineTransitionRequestSignalFadeIn(transition, 48000);
  std::vector<float> samples(4, 1.0f);
  audioPipelineTransitionApplyInputFadeIn(transition, samples.data(), 4, 1);
  if (!(samples[0] > 0.0f && samples[0] < samples[1] &&
        samples[1] < samples[2])) {
    fail("Discontinuity transition did not arm input fade-in after commit.");
  }
  std::fill(samples.begin(), samples.end(), 1.0f);
  audioPipelineTransitionApplyFadeIn(transition, samples.data(), 4, 1);
  if (!(samples[0] > 0.0f && samples[0] < samples[1] &&
        samples[1] < samples[2])) {
    fail("Discontinuity transition did not arm output fade-in after commit.");
  }
}

void expectSupersededDiscontinuityLeavesCommitInProgress() {
  AudioPipelineTransition transition;
  audioPipelineTransitionRequestDiscontinuity(transition, 48000);
  if (!audioPipelineTransitionBeginFadeOut(transition)) {
    fail("Superseded commit test did not begin fade-out.");
  }
  if (!audioPipelineTransitionBeginCommit(transition)) {
    fail("Superseded commit test did not begin commit.");
  }
  audioPipelineTransitionRequestDiscontinuity(transition, 48000);
  if (audioPipelineTransitionCommitInProgress(transition)) {
    fail("Superseded discontinuity left the old commit writable.");
  }
}

void expectSupersededDiscontinuityDoesNotClearNewRequest() {
  AudioPipelineTransition transition;
  audioPipelineTransitionRequestDiscontinuity(transition, 48000);
  if (!audioPipelineTransitionBeginFadeOut(transition)) {
    fail("First discontinuity did not begin fade-out.");
  }
  if (!audioPipelineTransitionBeginCommit(transition)) {
    fail("First discontinuity did not begin commit.");
  }
  audioPipelineTransitionRequestDiscontinuity(transition, 48000);
  if (audioPipelineTransitionFinishCommit(transition)) {
    fail("Superseded discontinuity incorrectly returned to idle.");
  }
  if (transition.phase.load(std::memory_order_relaxed) !=
      AudioPipelineTransitionPhase::FadeOutRequested) {
    fail("Superseded discontinuity did not leave the newer request pending.");
  }
}

void expectDryProducerClearsPendingInputFade() {
  AudioPipelineTransition transition;
  audioPipelineTransitionReset(transition);
  audioPipelineTransitionRequestSignalFadeIn(transition, 400);

  float first = 1.0f;
  audioPipelineTransitionApplyInputFadeIn(transition, &first, 1, 1);
  if (!(first > 0.0f && first < 1.0f)) {
    fail("Input fade did not start before dry-producer release.");
  }

  audioPipelineTransitionClearInputFadeIn(transition);
  float later[] = {1.0f, 1.0f};
  audioPipelineTransitionApplyInputFadeIn(transition, later, 2, 1);
  expectNear(later[0], 1.0f, 1e-6f, "cleared input fade frame 0");
  expectNear(later[1], 1.0f, 1e-6f, "cleared input fade frame 1");
}

void expectPlaybackSourcePrimingBudget() {
  const PlaybackSourcePriming priming = playbackSourcePrimingForRate(48000);
  if (priming.capacityFrames != 24000) {
    fail("Playback source capacity is not 500 ms at 48 kHz.");
  }
  if (priming.targetFrames != 12000) {
    fail("Playback source target is not 250 ms at 48 kHz.");
  }
  if (priming.primeFrames != 4800) {
    fail("Playback source prime is not 100 ms at 48 kHz.");
  }
  if (playbackSourceIsPrimed(priming.primeFrames - 1, priming.primeFrames,
                             false)) {
    fail("Playback source primed before the configured threshold.");
  }
  if (!playbackSourceIsPrimed(0, priming.primeFrames, true)) {
    fail("Playback source end did not satisfy priming.");
  }
}

void expectPlaybackSourceBufferWrapsCleanly() {
  PlaybackSourceBuffer buffer;
  buffer.init(4, 2);
  const float first[] = {1.0f, 1.1f, 2.0f, 2.1f, 3.0f, 3.1f};
  if (buffer.write(first, 3) != 3 || buffer.size() != 3 ||
      buffer.space() != 1) {
    fail("Playback source buffer did not accept the initial write.");
  }

  float out[8] = {};
  if (buffer.read(out, 2) != 2) {
    fail("Playback source buffer did not read the initial frames.");
  }
  expectNear(out[0], 1.0f, 1e-6f, "buffer read frame 0 left");
  expectNear(out[3], 2.1f, 1e-6f, "buffer read frame 1 right");

  const float second[] = {4.0f, 4.1f, 5.0f, 5.1f, 6.0f, 6.1f};
  if (buffer.write(second, 3) != 3 || buffer.size() != 4) {
    fail("Playback source buffer did not wrap on write.");
  }

  std::fill(out, out + 8, 0.0f);
  if (buffer.read(out, 4) != 4) {
    fail("Playback source buffer did not read wrapped frames.");
  }
  expectNear(out[0], 3.0f, 1e-6f, "buffer wrapped frame 0 left");
  expectNear(out[1], 3.1f, 1e-6f, "buffer wrapped frame 0 right");
  expectNear(out[2], 4.0f, 1e-6f, "buffer wrapped frame 1 left");
  expectNear(out[7], 6.1f, 1e-6f, "buffer wrapped frame 3 right");
}

void expectRadioBypassCrossfadesAtProducerBoundary() {
  RadioBypassTransition transition;
  transition.reset(false, 400);
  if (transition.needsWetSignal(false)) {
    fail("Dry radio bypass unexpectedly required a wet signal.");
  }
  if (!transition.activateWetSignal(true)) {
    fail("Radio enable did not activate the wet signal.");
  }

  float wet[] = {1.0f, 1.0f, 1.0f, 1.0f};
  const float dry[] = {0.0f, 0.0f, 0.0f, 0.0f};
  transition.blend(wet, dry, 4, 1, true, 400);
  expectNear(wet[0], 0.00f, 1e-6f, "radio enable frame 0");
  expectNear(wet[1], 0.25f, 1e-6f, "radio enable frame 1");
  expectNear(wet[2], 0.50f, 1e-6f, "radio enable frame 2");
  expectNear(wet[3], 0.75f, 1e-6f, "radio enable frame 3");
  if (transition.blendFramesRemaining(true, 400) != 0) {
    fail("Radio enable crossfade did not reach the wet signal.");
  }

  std::fill(std::begin(wet), std::end(wet), 1.0f);
  transition.blend(wet, dry, 4, 1, false, 400);
  expectNear(wet[0], 1.00f, 1e-6f, "radio disable frame 0");
  expectNear(wet[1], 0.75f, 1e-6f, "radio disable frame 1");
  expectNear(wet[2], 0.50f, 1e-6f, "radio disable frame 2");
  expectNear(wet[3], 0.25f, 1e-6f, "radio disable frame 3");
  if (transition.needsWetSignal(false)) {
    fail("Radio disable crossfade did not release the wet signal.");
  }
}

void expectRadioBypassCrossfadeIsBlockInvariant() {
  RadioBypassTransition whole;
  RadioBypassTransition split;
  whole.reset(false, 400);
  split.reset(false, 400);
  whole.activateWetSignal(true);
  split.activateWetSignal(true);

  float wholeWet[] = {0.9f, 0.8f, 0.7f, 0.6f,
                      0.5f, 0.4f, 0.3f, 0.2f};
  float splitWet[] = {0.9f, 0.8f, 0.7f, 0.6f,
                      0.5f, 0.4f, 0.3f, 0.2f};
  const float dry[] = {-0.2f, -0.1f, 0.0f, 0.1f,
                       0.2f, 0.3f, 0.4f, 0.5f};
  whole.blend(wholeWet, dry, 8, 1, true, 400);
  split.blend(splitWet, dry, 3, 1, true, 400);
  split.blend(splitWet + 3, dry + 3, 5, 1, true, 400);

  for (size_t i = 0; i < std::size(wholeWet); ++i) {
    expectNear(splitWet[i], wholeWet[i], 1e-6f,
               "radio bypass block invariance");
  }
  float wholeNext = 0.75f;
  float splitNext = wholeNext;
  const float dryNext = -0.25f;
  whole.blend(&wholeNext, &dryNext, 1, 1, false, 400);
  split.blend(&splitNext, &dryNext, 1, 1, false, 400);
  expectNear(splitNext, wholeNext, 1e-6f,
             "radio bypass continuation after split blocks");
}

}  // namespace

int main() {
  expectTransparentBelowCeiling();
  expectLinearAttenuationAboveCeiling();
  expectSlowReleaseAfterLimiting();
  expectDeclickRampIsLinearPerFrame();
  expectSignalFadeArmsInputAndOutputBoundaries();
  expectDefaultDeclickRampFollowsSampleRate();
  expectTailFadeOnlyTouchesAudioTail();
  expectRampBoundsStartStep();
  expectTailFadeBoundsStopStep();
  expectDiscontinuityTransitionPhases();
  expectSupersededDiscontinuityLeavesCommitInProgress();
  expectSupersededDiscontinuityDoesNotClearNewRequest();
  expectDryProducerClearsPendingInputFade();
  expectPlaybackSourcePrimingBudget();
  expectPlaybackSourceBufferWrapsCleanly();
  expectRadioBypassCrossfadesAtProducerBoundary();
  expectRadioBypassCrossfadeIsBlockInvariant();
  return 0;
}
