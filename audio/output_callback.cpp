#include "audioplayback_internal.h"

extern "C" {
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>

#include "clock.h"
#include "output_volume_safety.h"
#include "pipeline_transition.h"
#include "queued_audio_source.h"
#include "runtime_helpers.h"

namespace {

void updatePeakMeter(AudioState* state, const float* samples,
                     ma_uint32 frameCount) {
  if (!state || !samples || frameCount == 0) {
    if (state) {
      state->peak.store(0.0f, std::memory_order_relaxed);
    }
    return;
  }
  const uint32_t channels = std::max(1u, state->channels);
  const size_t count = static_cast<size_t>(frameCount) * channels;
  float peak = 0.0f;
  for (size_t i = 0; i < count; ++i) {
    peak = std::max(peak, std::fabs(samples[i]));
  }
  float prev = state->peak.load(std::memory_order_relaxed);
  float rate = static_cast<float>(std::max(1u, state->sampleRate));
  float decay = std::exp(-static_cast<float>(frameCount) / (rate * 0.30f));
  float next = std::max(peak, prev * decay);
  state->peak.store(next, std::memory_order_relaxed);
}

}  // namespace

void audioPlaybackHoldClipAlert(AudioState* state) {
  if (!state) return;
  constexpr int64_t kClipAlertHoldUs = 900000;
  state->clipAlertUntilUs.store(nowUs() + kClipAlertHoldUs,
                                std::memory_order_relaxed);
}

void dataCallback(ma_device* device, void* output, const void*,
                  ma_uint32 frameCount) {
  auto* state = static_cast<AudioState*>(device->pUserData);
  float* out = static_cast<float*>(output);
  if (!state) return;

  state->callbackCount.fetch_add(1, std::memory_order_relaxed);
  state->framesRequested.fetch_add(frameCount, std::memory_order_relaxed);
  const uint32_t previousCallbackFrames =
      state->lastCallbackFrames.exchange(frameCount, std::memory_order_relaxed);
  state->audioPrimed.store(true, std::memory_order_relaxed);

  if (state->sourcePreparing.load(std::memory_order_acquire)) {
    const uint32_t channels = state->channels;
    state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    std::fill(out, out + frameCount * channels, 0.0f);
    updatePeakMeter(state, out, frameCount);
    state->decodeCv.notify_all();
    state->m4aCv.notify_all();
    return;
  }

  bool useStreamQueue = state->streamQueueEnabled.load();
  if (useStreamQueue) {
    const uint32_t channels = state->channels;
    const bool paused = state->paused.load() || state->hold.load();
    if (paused) {
      if (audioPipelineTransitionBeginFadeOut(state->pipelineTransition)) {
        if (!queuedAudioSourceCommitPendingSerialFlush(state)) {
          state->decodeCv.notify_all();
          state->m4aCv.notify_all();
        }
      }
      state->pausedCallbacks.fetch_add(1, std::memory_order_relaxed);
      state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
      state->lastFramesRead.store(0, std::memory_order_relaxed);
      std::fill(out, out + frameCount * channels, 0.0f);
      updatePeakMeter(state, out, frameCount);
      state->audioClock.set_paused(true, nowUs());
      state->streamStarved.store(false, std::memory_order_relaxed);
      return;
    }

    if (audioPipelineTransitionWaitingForCommit(state->pipelineTransition)) {
      state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
      state->lastFramesRead.store(0, std::memory_order_relaxed);
      state->streamStarved.store(true, std::memory_order_relaxed);
      std::fill(out, out + frameCount * channels, 0.0f);
      updatePeakMeter(state, out, frameCount);
      state->decodeCv.notify_all();
      return;
    }

    uint64_t framesRead =
        state->streamRb.readSome(out, static_cast<uint64_t>(frameCount));
    if (framesRead < frameCount) {
      std::fill(out + framesRead * channels,
                out + frameCount * channels, 0.0f);
    }
    if (framesRead > 0) {
      state->decodeCv.notify_one();
    }

    state->framesPlayed.fetch_add(framesRead, std::memory_order_relaxed);
    state->framesReadTotal.fetch_add(framesRead, std::memory_order_relaxed);
    state->lastFramesRead.store(static_cast<uint32_t>(framesRead),
                                std::memory_order_relaxed);
    state->audioClockFrames.fetch_add(framesRead, std::memory_order_relaxed);
    if (framesRead < frameCount) {
      state->shortReadCount.fetch_add(1, std::memory_order_relaxed);
      state->silentFrames.fetch_add(frameCount - framesRead,
                                    std::memory_order_relaxed);
    }

    if (framesRead > 0) {
      uint64_t currentRpos =
          state->streamRb.rpos.load(std::memory_order_relaxed);
      int64_t currentPts = AV_NOPTS_VALUE;
      int currentSerial = state->streamSerial.load(std::memory_order_relaxed);

      {
        std::lock_guard<std::mutex> lock(state->streamMetadataMutex);
        while (state->streamMetadata.size() > 1 &&
               state->streamMetadata[1].wpos <= currentRpos) {
          state->streamMetadata.pop_front();
        }
        if (!state->streamMetadata.empty()) {
          const auto& md = state->streamMetadata.front();
          if (currentRpos >= md.wpos) {
            uint64_t offset = currentRpos - md.wpos;
            currentPts =
                md.ptsUs +
                static_cast<int64_t>(offset * 1000000ULL / state->sampleRate);
            currentSerial = md.serial;
          }
        }
      }

      if (currentPts != AV_NOPTS_VALUE) {
        uint64_t delay =
            state->deviceDelayFrames.load(std::memory_order_relaxed);
        int64_t delayUs =
            static_cast<int64_t>(delay * 1000000ULL / state->sampleRate);

        state->audioClock.set(currentPts - delayUs, nowUs(), currentSerial);
        state->streamClockReady.store(true, std::memory_order_relaxed);

        state->streamUpdateCounter.fetch_add(1, std::memory_order_release);
        state->streamUpdateCv.notify_all();
      }
    }

    bool starved = (framesRead < frameCount);
    const bool wasStreamStarved =
        state->streamStarved.exchange(starved, std::memory_order_relaxed);

    if (starved) {
      audioPipelineTransitionRequestOutputFadeIn(state->pipelineTransition,
                                                 state->sampleRate);
      int64_t now = nowUs();
      int64_t last =
          state->audioClock.last_updated_us.load(std::memory_order_relaxed);
      int64_t graceUs =
          queuedAudioSourceClockStarvationGraceUs(state, frameCount);
      if (now - last > graceUs) {
        state->audioClock.invalidate();
        state->streamClockReady.store(false, std::memory_order_relaxed);
      }
    }

    if (starved && state->sourceAtEnd.load()) {
      state->finished.store(true);
      uint64_t total = state->totalFrames.load();
      if (total > 0) {
        state->framesPlayed.store(total, std::memory_order_relaxed);
      }
    }

    const bool seekFadeOut =
        audioPipelineTransitionBeginFadeOut(state->pipelineTransition);
    if (!seekFadeOut && framesRead > 0 && framesRead < frameCount) {
      audioPipelineTransitionFadeTailToSilence(
          out, static_cast<uint32_t>(framesRead), channels, state->sampleRate);
    }

    if (seekFadeOut) {
      audioPipelineTransitionFadeOutToSilence(out, frameCount, channels,
                                              state->sampleRate);
      state->lastFramesRead.store(0, std::memory_order_relaxed);
      if (!queuedAudioSourceCommitPendingSerialFlush(state)) {
        state->decodeCv.notify_all();
        state->m4aCv.notify_all();
      }
    } else if (framesRead > 0) {
      if (wasStreamStarved) {
        audioPipelineTransitionRequestOutputFadeIn(state->pipelineTransition,
                                                   state->sampleRate);
      }
      audioPipelineTransitionApplyFadeIn(state->pipelineTransition, out,
                                         frameCount, channels);
    }

    float vol = state->volume.load(std::memory_order_relaxed);
    if (applyOutputVolumeSafety(out, frameCount, channels, vol,
                                state->sampleRate, state->outputSafety)) {
      audioPlaybackHoldClipAlert(state);
    }
    updatePeakMeter(state, out, frameCount);
    return;
  }

  if (state->hold.load()) {
    if (audioPipelineTransitionBeginFadeOut(state->pipelineTransition)) {
      if (!queuedAudioSourceCommitPendingSerialFlush(state)) {
        state->decodeCv.notify_all();
        state->m4aCv.notify_all();
      }
    }
    state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    std::fill(out, out + frameCount * state->channels, 0.0f);
    updatePeakMeter(state, out, frameCount);
    return;
  }

  const AudioBackendHandlers* backend = state->backend;
  const uint32_t previousFramesRead =
      state->lastFramesRead.load(std::memory_order_relaxed);
  bool seekDeclick = false;
  const bool seekFadeOut =
      audioPipelineTransitionBeginFadeOut(state->pipelineTransition);
  if (backend && backend->allowSeekInCallback && backend->seek &&
      !seekFadeOut && state->seekRequested.load(std::memory_order_relaxed) &&
      audioPipelineTransitionBeginCommit(state->pipelineTransition)) {
    int64_t target = state->pendingSeekFrames.load();
    if (target < 0) target = 0;
    if (!backend->seek(static_cast<uint64_t>(target))) {
      target = 0;
      backend->seek(0);
    }
    state->framesPlayed.store(static_cast<uint64_t>(target));
    state->framesRequested.store(static_cast<uint64_t>(target));
    state->audioClockFrames.store(static_cast<uint64_t>(target));
    state->finished.store(false);
    audioPlaybackFinishSeekPipelineTransition(state);
    seekDeclick = true;
  }

  if (!seekFadeOut &&
      audioPipelineTransitionWaitingForCommit(state->pipelineTransition)) {
    state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    std::fill(out, out + frameCount * state->channels, 0.0f);
    updatePeakMeter(state, out, frameCount);
    state->m4aCv.notify_all();
    state->decodeCv.notify_all();
    return;
  }

  if (state->paused.load()) {
    if (seekFadeOut) {
      if (!queuedAudioSourceCommitPendingSerialFlush(state)) {
        state->decodeCv.notify_all();
        state->m4aCv.notify_all();
      }
    }
    state->pausedCallbacks.fetch_add(1, std::memory_order_relaxed);
    state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    std::fill(out, out + frameCount * state->channels, 0.0f);
    return;
  }

  uint32_t framesRemaining = frameCount;
  uint64_t silentLeadFrames = 0;
  uint64_t leadSilence = state->audioLeadSilenceFrames.load();
  float* outCursor = out;
  if (leadSilence > 0) {
    silentLeadFrames = std::min<uint64_t>(leadSilence, framesRemaining);
    if (silentLeadFrames > 0) {
      std::fill(outCursor, outCursor + silentLeadFrames * state->channels,
                0.0f);
      state->audioLeadSilenceFrames.fetch_sub(silentLeadFrames,
                                              std::memory_order_relaxed);
      framesRemaining -= static_cast<uint32_t>(silentLeadFrames);
      outCursor += silentLeadFrames * state->channels;
    }
  }

  uint64_t framesRead = 0;
  auto finishPlayback = [&]() {
    state->finished.store(true, std::memory_order_relaxed);
    uint64_t total = state->totalFrames.load(std::memory_order_relaxed);
    if (total > 0) {
      state->framesPlayed.store(total, std::memory_order_relaxed);
    } else {
      state->framesPlayed.store(0, std::memory_order_relaxed);
    }
    std::fill(outCursor, outCursor + framesRemaining * state->channels, 0.0f);
    framesRead = 0;
  };
  if (framesRemaining > 0) {
    if (!backend || !backend->read) {
      finishPlayback();
    } else {
      uint64_t backendFramesRead = 0;
      if (!backend->read(outCursor, framesRemaining, &backendFramesRead)) {
        finishPlayback();
      } else {
        framesRead = backendFramesRead;
        if (framesRead < framesRemaining) {
          std::fill(outCursor + framesRead * state->channels,
                    outCursor + framesRemaining * state->channels, 0.0f);
        }
        if (framesRead == 0 && state->sourceAtEnd.load()) {
          state->finished.store(true, std::memory_order_relaxed);
          uint64_t total = state->totalFrames.load(std::memory_order_relaxed);
          if (total > 0) {
            state->framesPlayed.store(total, std::memory_order_relaxed);
          }
        } else if (backend->finishOnShortRead &&
                   (framesRead == 0 || framesRead < framesRemaining)) {
          finishPlayback();
        }
      }
    }
  }

  state->audioClockFrames.fetch_add(frameCount, std::memory_order_relaxed);
  const bool previousShortRead =
      previousCallbackFrames > 0 && previousFramesRead < previousCallbackFrames;
  const bool signalFadeIn =
      framesRead > 0 &&
      (seekDeclick || previousFramesRead == 0 || previousShortRead);
  if (signalFadeIn) {
    audioPipelineTransitionRequestOutputFadeIn(state->pipelineTransition,
                                               state->sampleRate);
  }
  state->framesPlayed.fetch_add(framesRead);
  state->framesReadTotal.fetch_add(framesRead, std::memory_order_relaxed);
  state->lastFramesRead.store(static_cast<uint32_t>(framesRead),
                              std::memory_order_relaxed);
  uint64_t silentFrames = silentLeadFrames;
  if (framesRead < framesRemaining) {
    silentFrames += static_cast<uint64_t>(framesRemaining - framesRead);
    state->shortReadCount.fetch_add(1, std::memory_order_relaxed);
  }
  if (silentFrames > 0) {
    state->silentFrames.fetch_add(silentFrames, std::memory_order_relaxed);
  }

  if (!seekFadeOut && framesRead > 0 && framesRead < framesRemaining) {
    float* audioStart =
        out + silentLeadFrames * static_cast<uint64_t>(state->channels);
    audioPipelineTransitionFadeTailToSilence(
        audioStart, static_cast<uint32_t>(framesRead), state->channels,
        state->sampleRate);
  }

  if (seekFadeOut) {
    audioPipelineTransitionFadeOutToSilence(out, frameCount, state->channels,
                                            state->sampleRate);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    if (!queuedAudioSourceCommitPendingSerialFlush(state)) {
      state->decodeCv.notify_all();
      state->m4aCv.notify_all();
    }
  } else if (framesRead > 0) {
    audioPipelineTransitionApplyFadeIn(state->pipelineTransition, out,
                                       frameCount, state->channels);
  }

  float vol = state->volume.load(std::memory_order_relaxed);
  if (applyOutputVolumeSafety(out, frameCount, state->channels, vol,
                              state->sampleRate, state->outputSafety)) {
    audioPlaybackHoldClipAlert(state);
  }
  updatePeakMeter(state, out, frameCount);
}
