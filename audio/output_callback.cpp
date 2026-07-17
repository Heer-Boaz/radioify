#include "audioplayback_internal.h"

extern "C" {
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "output_volume_safety.h"
#include "pipeline_transition.h"
#include "queued_audio_source.h"
#include "runtime_helpers.h"

namespace {

void updatePeakMeter(AudioState* state, const float* samples,
                     ma_uint32 frameCount) {
  if (!samples || frameCount == 0) {
    state->peak.store(0.0f, std::memory_order_relaxed);
    return;
  }
  const uint32_t channels = std::max(1u, state->channels);
  const size_t count = static_cast<size_t>(frameCount) * channels;
  float peak = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    peak = std::max(peak, std::fabs(samples[i]));
  }
  const float previous = state->peak.load(std::memory_order_relaxed);
  const float rate = static_cast<float>(std::max(1u, state->sampleRate));
  const float decay =
      std::exp(-static_cast<float>(frameCount) / (rate * 0.30f));
  state->peak.store(std::max(peak, previous * decay),
                    std::memory_order_relaxed);
}

void outputSilence(AudioState* state,
                   float* output,
                   uint32_t frameCount) {
  state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
  state->lastFramesRead.store(0, std::memory_order_relaxed);
  std::fill(output,
            output + static_cast<size_t>(frameCount) * state->channels,
            0.0f);
  updatePeakMeter(state, output, frameCount);
}

void updateStreamClock(AudioState* state) {
  const uint64_t currentReadPosition =
      state->processedAudio.readPosition();
  int64_t currentPtsUs = AV_NOPTS_VALUE;
  int currentSerial = state->streamSerial.load(std::memory_order_relaxed);
  AudioTimelineAnchor anchor;
  while (state->processedTimeline.peek(1, &anchor) &&
         anchor.framePosition <= currentReadPosition) {
    state->processedTimeline.popFront();
  }
  if (state->processedTimeline.peek(0, &anchor) &&
      currentReadPosition >= anchor.framePosition) {
    const uint64_t offset = currentReadPosition - anchor.framePosition;
    currentPtsUs =
        anchor.ptsUs + static_cast<int64_t>(offset * 1000000ULL /
                                            state->sampleRate);
    currentSerial = anchor.serial;
  }
  if (currentPtsUs == AV_NOPTS_VALUE) return;

  const uint64_t delayFrames =
      state->deviceDelayFrames.load(std::memory_order_relaxed);
  const int64_t delayUs = static_cast<int64_t>(
      delayFrames * 1000000ULL / state->sampleRate);
  state->audioClock.set(currentPtsUs - delayUs, nowUs(), currentSerial);
  state->streamClockReady.store(true, std::memory_order_relaxed);
  state->streamUpdateCounter.fetch_add(1, std::memory_order_release);
  state->streamUpdateCv.notify_all();
}

}  // namespace

void dataCallback(ma_device* device, void* output, const void*,
                  ma_uint32 frameCount) {
  auto* state = static_cast<AudioState*>(device->pUserData);
  auto* out = static_cast<float*>(output);
  if (!state) return;

  state->callbackCount.fetch_add(1, std::memory_order_relaxed);
  state->framesRequested.fetch_add(frameCount, std::memory_order_relaxed);
  state->lastCallbackFrames.store(frameCount, std::memory_order_relaxed);
  state->audioPrimed.store(true, std::memory_order_relaxed);

  if (state->sourcePreparing.load(std::memory_order_acquire)) {
    outputSilence(state, out, frameCount);
    state->audioQueueCv.notify_all();
    return;
  }
  if (!state->streamQueueEnabled.load(std::memory_order_acquire)) {
    outputSilence(state, out, frameCount);
    return;
  }

  const uint32_t channels = state->channels;
  if (state->paused.load(std::memory_order_relaxed) ||
      state->hold.load(std::memory_order_relaxed)) {
    if (audioPipelineTransitionBeginFadeOut(state->pipelineTransition)) {
      state->audioQueueCv.notify_all();
      state->radioDspCv.notify_all();
    }
    state->pausedCallbacks.fetch_add(1, std::memory_order_relaxed);
    outputSilence(state, out, frameCount);
    state->audioClock.set_paused(true, nowUs());
    state->streamStarved.store(false, std::memory_order_relaxed);
    return;
  }

  if (audioPipelineTransitionWaitingForCommit(state->pipelineTransition)) {
    outputSilence(state, out, frameCount);
    state->streamStarved.store(true, std::memory_order_relaxed);
    state->radioDspCv.notify_all();
    return;
  }

  const uint64_t leadSilence =
      state->audioLeadSilenceFrames.load(std::memory_order_relaxed);
  const uint32_t silentLeadFrames = static_cast<uint32_t>(
      std::min<uint64_t>(leadSilence, frameCount));
  if (silentLeadFrames > 0) {
    std::fill(out,
              out + static_cast<size_t>(silentLeadFrames) * channels,
              0.0f);
    state->audioLeadSilenceFrames.fetch_sub(silentLeadFrames,
                                            std::memory_order_relaxed);
  }

  const uint32_t requestedAudioFrames = frameCount - silentLeadFrames;
  float* audioOutput =
      out + static_cast<size_t>(silentLeadFrames) * channels;
  const uint64_t framesRead =
      requestedAudioFrames > 0
          ? state->processedAudio.readSome(audioOutput, requestedAudioFrames)
          : 0;
  if (framesRead < requestedAudioFrames) {
    std::fill(audioOutput + framesRead * channels,
              audioOutput +
                  static_cast<size_t>(requestedAudioFrames) * channels,
              0.0f);
  }
  state->framesPlayed.fetch_add(framesRead, std::memory_order_relaxed);
  state->framesReadTotal.fetch_add(framesRead, std::memory_order_relaxed);
  state->lastFramesRead.store(static_cast<uint32_t>(framesRead),
                              std::memory_order_relaxed);
  state->audioClockFrames.fetch_add(silentLeadFrames + framesRead,
                                    std::memory_order_relaxed);
  if (silentLeadFrames > 0) {
    state->silentFrames.fetch_add(silentLeadFrames,
                                  std::memory_order_relaxed);
  }
  if (framesRead < requestedAudioFrames) {
    state->shortReadCount.fetch_add(1, std::memory_order_relaxed);
    state->silentFrames.fetch_add(requestedAudioFrames - framesRead,
                                  std::memory_order_relaxed);
  }
  if (framesRead > 0) updateStreamClock(state);
  state->radioDspCv.notify_one();

  const bool starved = framesRead < requestedAudioFrames;
  const bool wasStarved =
      state->streamStarved.exchange(starved, std::memory_order_relaxed);
  if (starved) {
    audioPipelineTransitionRequestOutputFadeIn(state->pipelineTransition,
                                               state->sampleRate);
    const int64_t currentTimeUs = nowUs();
    const int64_t lastClockUpdateUs =
        state->audioClock.last_updated_us.load(std::memory_order_relaxed);
    if (currentTimeUs - lastClockUpdateUs >
        queuedAudioSourceClockStarvationGraceUs(state, frameCount)) {
      state->audioClock.invalidate();
      state->streamClockReady.store(false, std::memory_order_relaxed);
    }
  }
  if (starved &&
      state->processedAtEnd.load(std::memory_order_relaxed)) {
    state->finished.store(true, std::memory_order_relaxed);
    const uint64_t total = state->totalFrames.load(std::memory_order_relaxed);
    if (total > 0) {
      state->framesPlayed.store(total, std::memory_order_relaxed);
    }
  }

  const bool fadeOut =
      audioPipelineTransitionBeginFadeOut(state->pipelineTransition);
  if (!fadeOut && framesRead > 0 && framesRead < requestedAudioFrames) {
    audioPipelineTransitionFadeTailToSilence(
        audioOutput, static_cast<uint32_t>(framesRead), channels,
        state->sampleRate);
  }
  if (fadeOut) {
    audioPipelineTransitionFadeOutToSilence(out, frameCount, channels,
                                            state->sampleRate);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    state->audioQueueCv.notify_all();
    state->radioDspCv.notify_all();
  } else if (framesRead > 0) {
    if (wasStarved) {
      audioPipelineTransitionRequestOutputFadeIn(state->pipelineTransition,
                                                 state->sampleRate);
    }
    audioPipelineTransitionApplyFadeIn(state->pipelineTransition, out,
                                       frameCount, channels);
  }

  const float volume = state->volume.load(std::memory_order_relaxed);
  applyOutputVolumeSafety(out, frameCount, channels, volume,
                          state->sampleRate, state->outputSafety);
  updatePeakMeter(state, out, frameCount);
}
