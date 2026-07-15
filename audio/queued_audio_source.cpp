#include "queued_audio_source.h"

extern "C" {
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include "audioplayback_internal.h"
#include "pipeline_transition.h"
#include "playback_source_priming.h"

namespace {

constexpr uint32_t kRadioDspChunkFrames = 2048;
constexpr uint64_t kDecodedAudioHandoffFrames = 4096;

enum class ProcessingCommit {
  None,
  SourceSeek,
  StreamReset,
};

bool queuedDecodeReachedKnownEnd(const AudioState* state, uint64_t framePos) {
  const uint64_t totalFrames =
      state->totalFrames.load(std::memory_order_relaxed);
  return totalFrames > 0 && framePos >= totalFrames;
}

bool transitionCommitIsCurrent(const AudioPipelineTransition& transition) {
  return audioPipelineTransitionCommitInProgress(transition) &&
         transition.commitSerial.load(std::memory_order_acquire) ==
             transition.requestSerial.load(std::memory_order_acquire);
}

int64_t timelinePtsAt(const AudioTimelineAnchor& anchor,
                      uint64_t framePosition,
                      uint32_t sampleRate) {
  assert(framePosition >= anchor.framePosition && sampleRate > 0);
  return anchor.ptsUs +
         static_cast<int64_t>((framePosition - anchor.framePosition) *
                              1000000ULL / sampleRate);
}

void appendTimelineAnchor(SpscAudioTimeline* timeline,
                          uint64_t writePosition,
                          int64_t ptsUs,
                          int serial) {
  if (!timeline->append({writePosition, ptsUs, serial})) std::abort();
}

void clearAudioTimelines(AudioState* state) {
  state->decodedTimeline.discardAnchors();
  state->processedTimeline.discardAnchors();
}

void resetQueuedTransport(AudioState* state,
                          uint64_t framePosition,
                          int serial,
                          int64_t discardUntilUs,
                          bool resetPlaybackPosition) {
  {
    std::lock_guard<std::mutex> queueLock(state->audioQueueMutex);
    state->decodedAudio.discardBufferedFrames();
    state->processedAudio.discardBufferedFrames();
    clearAudioTimelines(state);
    state->streamSerial.store(serial, std::memory_order_release);
    state->streamDiscardPtsUs.store((std::max)(int64_t{0}, discardUntilUs),
                                    std::memory_order_relaxed);
  }

  if (resetPlaybackPosition) {
    state->framesPlayed.store(framePosition, std::memory_order_relaxed);
    state->audioClockFrames.store(framePosition, std::memory_order_relaxed);
    state->pendingSeekFrames.store(static_cast<int64_t>(framePosition),
                                   std::memory_order_relaxed);
  }
  state->finished.store(false, std::memory_order_relaxed);
  state->sourceAtEnd.store(false, std::memory_order_relaxed);
  state->processedAtEnd.store(false, std::memory_order_relaxed);
  state->audioPrimed.store(false, std::memory_order_relaxed);
  state->streamClockReady.store(false, std::memory_order_relaxed);
  state->streamStarved.store(false, std::memory_order_relaxed);
  state->audioClock.reset(serial);
  state->radioFilter.requestReset();
  audioPipelineTransitionRequestSignalFadeIn(state->pipelineTransition,
                                             state->sampleRate);
  state->audioQueueCv.notify_all();
  state->radioDspCv.notify_all();
}

bool sourceResetIsPending(const AudioState* state) {
  return state->sourceResetRequestGeneration.load(std::memory_order_acquire) >
         state->sourceResetAppliedGeneration.load(std::memory_order_acquire);
}

bool streamResetCanBegin(const AudioState* state) {
  return state->externalStream.load(std::memory_order_relaxed) &&
         state->streamResetRequestedGeneration.load(
             std::memory_order_acquire) >
             state->streamResetAppliedGeneration.load(
                 std::memory_order_acquire) &&
         state->pipelineTransition.phase.load(std::memory_order_acquire) ==
             AudioPipelineTransitionPhase::CommitReady;
}

bool applyPendingSourceReset(AudioState* state,
                             ProcessingCommit* processingCommit) {
  const uint64_t requested =
      state->sourceResetRequestGeneration.load(std::memory_order_acquire);
  if (requested <= state->sourceResetAppliedGeneration.load(
                       std::memory_order_acquire)) {
    return false;
  }

  if (transitionCommitIsCurrent(state->pipelineTransition)) {
    resetQueuedTransport(
        state,
        state->sourceResetFramePosition.load(std::memory_order_relaxed),
        state->streamSerial.load(std::memory_order_relaxed), 0, true);
    *processingCommit = ProcessingCommit::SourceSeek;
  }
  state->sourceResetAppliedGeneration.store(requested,
                                            std::memory_order_release);
  state->audioQueueCv.notify_all();
  return true;
}

bool applyPendingStreamReset(AudioState* state,
                             ProcessingCommit* processingCommit) {
  if (!streamResetCanBegin(state) ||
      !audioPipelineTransitionBeginCommit(state->pipelineTransition)) {
    return false;
  }

  std::lock_guard<std::mutex> resetLock(state->streamResetMutex);
  const uint64_t requested = state->streamResetRequestedGeneration.load(
      std::memory_order_acquire);
  if (requested <= state->streamResetAppliedGeneration.load(
                       std::memory_order_acquire) ||
      state->pendingStreamReset.generation != requested ||
      !transitionCommitIsCurrent(state->pipelineTransition)) {
    return true;
  }

  const AudioStreamResetRequest request = state->pendingStreamReset;
  resetQueuedTransport(state, request.framePosition, request.serial,
                       request.discardUntilUs,
                       request.resetPlaybackPosition);
  state->streamResetAppliedGeneration.store(request.generation,
                                            std::memory_order_release);
  *processingCommit = ProcessingCommit::StreamReset;
  state->audioQueueCv.notify_all();
  return true;
}

void finishProcessingCommitWhenPrimed(AudioState* state,
                                      uint32_t primeFrames,
                                      ProcessingCommit* processingCommit) {
  if (*processingCommit == ProcessingCommit::None) return;
  if (!transitionCommitIsCurrent(state->pipelineTransition)) {
    *processingCommit = ProcessingCommit::None;
    return;
  }
  if (!playbackSourceIsPrimed(
          state->processedAudio.bufferedFrames(), primeFrames,
          state->processedAtEnd.load(std::memory_order_relaxed))) {
    return;
  }

  bool finished = false;
  if (*processingCommit == ProcessingCommit::SourceSeek) {
    finished = audioPlaybackFinishSeekPipelineTransition(state);
  } else {
    finished =
        audioPipelineTransitionFinishCommit(state->pipelineTransition);
  }
  if (finished || !transitionCommitIsCurrent(state->pipelineTransition)) {
    *processingCommit = ProcessingCommit::None;
  }
  state->audioQueueCv.notify_all();
}

void publishProcessedAudio(AudioState* state,
                           const float* samples,
                           uint64_t inputPosition,
                           uint64_t frames) {
  const uint64_t outputPosition = state->processedAudio.writePosition();
  const uint64_t inputEnd = inputPosition + frames;

  AudioTimelineAnchor anchor;
  while (state->decodedTimeline.peek(1, &anchor) &&
         anchor.framePosition <= inputPosition) {
    state->decodedTimeline.popFront();
  }
  uint64_t anchorOffset = 0;
  if (state->decodedTimeline.peek(0, &anchor) &&
      anchor.framePosition <= inputPosition) {
    appendTimelineAnchor(&state->processedTimeline, outputPosition,
                         timelinePtsAt(anchor, inputPosition,
                                       state->sampleRate),
                         anchor.serial);
    anchorOffset = 1;
  }
  while (state->decodedTimeline.peek(anchorOffset, &anchor)) {
    if (anchor.framePosition >= inputEnd) break;
    if (anchor.framePosition > inputPosition) {
      appendTimelineAnchor(
          &state->processedTimeline,
          outputPosition + anchor.framePosition - inputPosition,
          anchor.ptsUs, anchor.serial);
    }
    ++anchorOffset;
  }

  if (state->processedAudio.writeSome(samples, frames) != frames) {
    std::abort();
  }
  while (state->decodedTimeline.peek(1, &anchor) &&
         anchor.framePosition < inputEnd) {
    state->decodedTimeline.popFront();
  }
}

void radioDspMain(AudioState* state,
                  uint32_t outputTargetFrames,
                  uint32_t primeFrames) {
  std::vector<float> scratch(
      static_cast<size_t>(kRadioDspChunkFrames) * state->channels);
  ProcessingCommit processingCommit = ProcessingCommit::None;

  while (!state->radioDspStop.load(std::memory_order_relaxed)) {
    if (applyPendingSourceReset(state, &processingCommit)) {
      continue;
    }
    if (applyPendingStreamReset(state, &processingCommit)) {
      continue;
    }
    finishProcessingCommitWhenPrimed(state, primeFrames, &processingCommit);

    if (processingCommit == ProcessingCommit::None &&
        audioPipelineTransitionActive(state->pipelineTransition)) {
      std::unique_lock<std::mutex> lock(state->audioQueueMutex);
      state->radioDspCv.wait(lock, [&]() {
        return state->radioDspStop.load(std::memory_order_relaxed) ||
               sourceResetIsPending(state) || streamResetCanBegin(state) ||
               !audioPipelineTransitionActive(state->pipelineTransition);
      });
      continue;
    }

    const uint64_t inputFrames = state->decodedAudio.bufferedFrames();
    const uint64_t outputFrames = state->processedAudio.bufferedFrames();
    const uint64_t outputWritable = state->processedAudio.writableFrames();
    if (inputFrames == 0 || outputWritable == 0 ||
        outputFrames >= outputTargetFrames) {
      if (inputFrames == 0 &&
          state->sourceAtEnd.load(std::memory_order_relaxed)) {
        if (!state->processedAtEnd.exchange(true,
                                            std::memory_order_relaxed)) {
          state->audioQueueCv.notify_all();
        }
        finishProcessingCommitWhenPrimed(state, primeFrames,
                                         &processingCommit);
      }
      std::unique_lock<std::mutex> lock(state->audioQueueMutex);
      state->radioDspCv.wait(lock, [&]() {
        if (state->radioDspStop.load(std::memory_order_relaxed) ||
            sourceResetIsPending(state) || streamResetCanBegin(state)) {
          return true;
        }
        if (processingCommit != ProcessingCommit::None &&
            !transitionCommitIsCurrent(state->pipelineTransition)) {
          return true;
        }
        if (processingCommit == ProcessingCommit::None &&
            audioPipelineTransitionActive(state->pipelineTransition)) {
          return true;
        }
        const uint64_t queuedInput =
            state->decodedAudio.bufferedFrames();
        const uint64_t queuedOutput =
            state->processedAudio.bufferedFrames();
        const bool canProcess =
            queuedInput > 0 &&
            state->processedAudio.writableFrames() > 0 &&
            queuedOutput < outputTargetFrames;
        const bool canPublishEnd =
            queuedInput == 0 &&
            state->sourceAtEnd.load(std::memory_order_relaxed) &&
            !state->processedAtEnd.load(std::memory_order_relaxed);
        return canProcess || canPublishEnd;
      });
      continue;
    }

    const uint64_t targetWritable = outputTargetFrames - outputFrames;
    const uint64_t framesToProcess =
        std::min({inputFrames, outputWritable, targetWritable,
                  static_cast<uint64_t>(kRadioDspChunkFrames)});
    const uint64_t inputPosition = state->decodedAudio.readPosition();
    const uint64_t read =
        state->decodedAudio.readSome(scratch.data(), framesToProcess);
    assert(read == framesToProcess);
    state->audioQueueCv.notify_all();

    if (sourceResetIsPending(state) || streamResetCanBegin(state) ||
        state->radioDspStop.load(std::memory_order_relaxed)) {
      continue;
    }

    if (!state->dry &&
        state->radioFilter.process(scratch.data(),
                                   static_cast<uint32_t>(read),
                                   state->channels,
                                   state->pipelineTransition)) {
      audioPlaybackHoldClipAlert(state);
    }
    if (sourceResetIsPending(state) || streamResetCanBegin(state) ||
        state->radioDspStop.load(std::memory_order_relaxed)) {
      continue;
    }

    publishProcessedAudio(state, scratch.data(), inputPosition, read);
    state->processedAtEnd.store(false, std::memory_order_relaxed);
    state->audioQueueCv.notify_all();
    finishProcessingCommitWhenPrimed(state, primeFrames, &processingCommit);
  }

  state->radioDspThreadRunning.store(false, std::memory_order_release);
  state->audioQueueCv.notify_all();
}

bool queuedSourceWriteBlockedByTransition(
    const AudioState* state,
    bool allowCommitInProgressWrite) {
  if (state->externalStream.load(std::memory_order_relaxed)) {
    return false;
  }
  if (!audioPipelineTransitionActive(state->pipelineTransition)) {
    return false;
  }
  return !allowCommitInProgressWrite ||
         !audioPipelineTransitionCommitInProgress(
             state->pipelineTransition);
}

bool queuedAudioSourceWriteInternal(AudioState* state,
                                    const float* interleaved,
                                    uint64_t frames,
                                    int64_t ptsUs,
                                    int serial,
                                    bool allowBlock,
                                    bool allowCommitInProgressWrite,
                                    uint64_t* writtenFrames) {
  if (writtenFrames) *writtenFrames = 0;
  if (!state || !interleaved || frames == 0 ||
      !state->streamQueueEnabled.load(std::memory_order_acquire)) {
    return false;
  }
  if (serial != state->streamSerial.load(std::memory_order_acquire)) {
    return true;
  }

  uint64_t remaining = frames;
  uint64_t totalWritten = 0;
  while (remaining > 0) {
    if (state->decodeStop.load(std::memory_order_relaxed) ||
        state->m4aStop.load(std::memory_order_relaxed) ||
        state->radioDspStop.load(std::memory_order_relaxed)) {
      return false;
    }
    if (queuedSourceWriteBlockedByTransition(
            state, allowCommitInProgressWrite)) {
      return false;
    }
    if (!state->streamQueueEnabled.load(std::memory_order_acquire)) {
      return false;
    }
    if (serial != state->streamSerial.load(std::memory_order_acquire)) {
      return true;
    }

    std::unique_lock<std::mutex> queueLock(state->audioQueueMutex);
    if (state->decodeStop.load(std::memory_order_relaxed) ||
        state->m4aStop.load(std::memory_order_relaxed) ||
        state->radioDspStop.load(std::memory_order_relaxed) ||
        queuedSourceWriteBlockedByTransition(
            state, allowCommitInProgressWrite) ||
        !state->streamQueueEnabled.load(std::memory_order_acquire)) {
      return false;
    }
    if (serial != state->streamSerial.load(std::memory_order_acquire)) {
      return true;
    }

    const int64_t chunkPtsUs =
        ptsUs == AV_NOPTS_VALUE
            ? AV_NOPTS_VALUE
            : ptsUs + static_cast<int64_t>(totalWritten * 1000000ULL /
                                           state->sampleRate);
    const int64_t discardThreshold =
        state->streamDiscardPtsUs.load(std::memory_order_relaxed);
    if (discardThreshold > 0 && chunkPtsUs < discardThreshold) {
      if (writtenFrames) *writtenFrames = frames;
      return true;
    }
    if (discardThreshold > 0) {
      state->streamDiscardPtsUs.store(0, std::memory_order_relaxed);
    }

    const uint64_t writable = state->decodedAudio.writableFrames();
    if (writable == 0) {
      if (!allowBlock) break;
      state->audioQueueCv.wait(queueLock, [&]() {
        return state->decodeStop.load(std::memory_order_relaxed) ||
               state->m4aStop.load(std::memory_order_relaxed) ||
               state->radioDspStop.load(std::memory_order_relaxed) ||
               queuedSourceWriteBlockedByTransition(
                   state, allowCommitInProgressWrite) ||
               !state->streamQueueEnabled.load(std::memory_order_relaxed) ||
               serial !=
                   state->streamSerial.load(std::memory_order_relaxed) ||
               state->decodedAudio.writableFrames() > 0;
      });
      continue;
    }

    const uint64_t framesToWrite = std::min(remaining, writable);
    const uint64_t inputPosition = state->decodedAudio.writePosition();
    if (chunkPtsUs != AV_NOPTS_VALUE) {
      appendTimelineAnchor(&state->decodedTimeline, inputPosition,
                           chunkPtsUs, serial);
    }
    const uint64_t written = state->decodedAudio.writeSome(
        interleaved + static_cast<size_t>(totalWritten) * state->channels,
        framesToWrite);
    assert(written == framesToWrite);
    queueLock.unlock();

    remaining -= written;
    totalWritten += written;
    state->processedAtEnd.store(false, std::memory_order_relaxed);
    state->radioDspCv.notify_one();
  }

  if (writtenFrames) *writtenFrames = totalWritten;
  return true;
}

bool waitForQueuedDecodeRetry(AudioState* state) {
  std::unique_lock<std::mutex> lock(state->audioQueueMutex);
  state->audioQueueCv.wait_for(lock, std::chrono::milliseconds(5), [&]() {
    return state->decodeStop.load(std::memory_order_relaxed) ||
           audioPipelineTransitionActive(state->pipelineTransition) ||
           !state->streamQueueEnabled.load(std::memory_order_relaxed);
  });
  return !state->decodeStop.load(std::memory_order_relaxed) &&
         !audioPipelineTransitionActive(state->pipelineTransition) &&
         state->streamQueueEnabled.load(std::memory_order_relaxed);
}

}  // namespace

bool queuedAudioSourceUsesDecoderWorker(
    const AudioBackendHandlers* backend) {
  return backend && backend->mode != AudioMode::None &&
         backend->mode != AudioMode::Stream &&
         backend->mode != AudioMode::M4a;
}

void queuedAudioSourceStartProcessing(AudioState* state,
                                      uint64_t outputCapacityFrames,
                                      uint32_t outputTargetFrames,
                                      uint32_t primeFrames,
                                      uint64_t framePosition,
                                      int serial) {
  assert(state && outputCapacityFrames > 0 && outputTargetFrames > 0 &&
         outputTargetFrames <= outputCapacityFrames);
  assert(!state->radioDspThread.joinable());

  state->decodedAudio.initialize(
      std::min(outputCapacityFrames, kDecodedAudioHandoffFrames),
      state->channels);
  state->processedAudio.initialize(outputCapacityFrames, state->channels);
  state->decodedTimeline.initialize(
      state->decodedAudio.capacityFrames() * 2 + 1);
  state->processedTimeline.initialize(
      state->processedAudio.capacityFrames() * 2 + 1);
  state->sourceResetRequestGeneration.store(0, std::memory_order_relaxed);
  state->sourceResetAppliedGeneration.store(0, std::memory_order_relaxed);
  state->sourceResetFramePosition.store(framePosition,
                                        std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> resetLock(state->streamResetMutex);
    state->pendingStreamReset = {};
    state->streamResetRequestedGeneration.store(0,
                                                std::memory_order_relaxed);
    state->streamResetAppliedGeneration.store(0,
                                              std::memory_order_relaxed);
  }
  state->radioFilter.resetSource(state->channels);
  state->streamQueueEnabled.store(true, std::memory_order_release);
  resetQueuedTransport(state, framePosition, serial, 0, true);
  state->radioDspStop.store(false, std::memory_order_relaxed);
  state->radioDspThreadRunning.store(true, std::memory_order_release);
  state->radioDspThread =
      std::thread(radioDspMain, state, outputTargetFrames, primeFrames);
}

void queuedAudioSourceStopProcessing(AudioState* state) {
  if (!state) return;
  state->streamQueueEnabled.store(false, std::memory_order_release);
  state->radioDspStop.store(true, std::memory_order_relaxed);
  state->radioDspCv.notify_all();
  state->audioQueueCv.notify_all();
  if (state->radioDspThread.joinable()) {
    state->radioDspThread.join();
  }
  state->radioDspThreadRunning.store(false, std::memory_order_relaxed);
  state->radioDspStop.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> queueLock(state->audioQueueMutex);
    state->decodedAudio.discardBufferedFrames();
    state->processedAudio.discardBufferedFrames();
    clearAudioTimelines(state);
  }
}

void queuedAudioSourceStopDecoderWorker(AudioState* state) {
  if (!state) return;
  state->decodeStop.store(true, std::memory_order_relaxed);
  state->audioQueueCv.notify_all();
  if (state->decodeThread.joinable()) {
    state->decodeThread.join();
  }
  state->decodeThreadRunning.store(false, std::memory_order_relaxed);
  state->decodeStop.store(false, std::memory_order_relaxed);
}

bool queuedAudioSourceWaitPrimed(AudioState* state, uint32_t primeFrames) {
  if (!state) return false;
  std::unique_lock<std::mutex> lock(state->audioQueueMutex);
  state->audioQueueCv.wait(lock, [&]() {
    return playbackSourceIsPrimed(
               state->processedAudio.bufferedFrames(), primeFrames,
               state->processedAtEnd.load(std::memory_order_relaxed)) ||
           !state->radioDspThreadRunning.load(std::memory_order_acquire) ||
           !state->streamQueueEnabled.load(std::memory_order_acquire);
  });
  return playbackSourceIsPrimed(
      state->processedAudio.bufferedFrames(), primeFrames,
      state->processedAtEnd.load(std::memory_order_relaxed));
}

uint64_t queuedAudioSourceRequestSourceReset(AudioState* state,
                                             uint64_t framePosition) {
  assert(state && transitionCommitIsCurrent(state->pipelineTransition));
  state->sourceResetFramePosition.store(framePosition,
                                        std::memory_order_relaxed);
  const uint64_t generation =
      state->sourceResetRequestGeneration.fetch_add(
          1, std::memory_order_release) +
      1;
  state->radioDspCv.notify_all();
  return generation;
}

bool queuedAudioSourceWaitForSourceReset(AudioState* state,
                                         uint64_t generation) {
  assert(state && generation > 0);
  std::unique_lock<std::mutex> lock(state->audioQueueMutex);
  state->audioQueueCv.wait(lock, [&]() {
    return state->sourceResetAppliedGeneration.load(
               std::memory_order_acquire) >= generation ||
           state->decodeStop.load(std::memory_order_relaxed) ||
           state->m4aStop.load(std::memory_order_relaxed) ||
           state->radioDspStop.load(std::memory_order_relaxed) ||
           !state->streamQueueEnabled.load(std::memory_order_acquire);
  });
  return state->sourceResetAppliedGeneration.load(
             std::memory_order_acquire) >= generation;
}

uint64_t queuedAudioSourceRequestStreamReset(AudioState* state,
                                             int serial,
                                             int64_t discardUntilUs,
                                             uint64_t framePosition,
                                             bool resetPlaybackPosition) {
  assert(state && state->externalStream.load(std::memory_order_relaxed));
  std::lock_guard<std::mutex> resetLock(state->streamResetMutex);
  const uint64_t generation = state->pendingStreamReset.generation + 1;
  state->pendingStreamReset = {
      generation, serial, (std::max)(int64_t{0}, discardUntilUs),
      framePosition, resetPlaybackPosition};
  audioPipelineTransitionRequestDiscontinuity(state->pipelineTransition,
                                              state->sampleRate);
  state->streamResetRequestedGeneration.store(generation,
                                              std::memory_order_release);
  state->radioDspCv.notify_all();
  return generation;
}

bool queuedAudioSourceWaitForStreamReset(AudioState* state,
                                         uint64_t generation) {
  assert(state && generation > 0);
  std::unique_lock<std::mutex> lock(state->audioQueueMutex);
  state->audioQueueCv.wait(lock, [&]() {
    return state->streamResetAppliedGeneration.load(
               std::memory_order_acquire) >= generation ||
           state->radioDspStop.load(std::memory_order_relaxed) ||
           !state->streamQueueEnabled.load(std::memory_order_acquire);
  });
  return state->streamResetAppliedGeneration.load(
             std::memory_order_acquire) >= generation;
}

void queuedAudioSourceCancelStreamResets(AudioState* state) {
  assert(state);
  std::lock_guard<std::mutex> resetLock(state->streamResetMutex);
  state->streamResetAppliedGeneration.store(
      state->streamResetRequestedGeneration.load(std::memory_order_acquire),
      std::memory_order_release);
  state->audioQueueCv.notify_all();
}

int64_t queuedAudioSourceClockStarvationGraceUs(const AudioState* state,
                                                uint32_t frameCount) {
  constexpr int64_t kMinClockStarvationGraceUs = 150000;
  constexpr int64_t kMaxClockStarvationGraceUs = 2000000;
  if (!state || state->sampleRate == 0) {
    return kMinClockStarvationGraceUs;
  }

  uint64_t bufferedFrames =
      state->deviceDelayFrames.load(std::memory_order_relaxed);
  if (frameCount > 0) {
    bufferedFrames = std::max<uint64_t>(
        bufferedFrames, static_cast<uint64_t>(frameCount) * 2ULL);
  }
  if (bufferedFrames == 0) return kMinClockStarvationGraceUs;

  const int64_t bufferedUs = static_cast<int64_t>(
      bufferedFrames * 1000000ULL / state->sampleRate);
  if (bufferedUs <= 0) return kMinClockStarvationGraceUs;
  return std::clamp(bufferedUs * 2, kMinClockStarvationGraceUs,
                    kMaxClockStarvationGraceUs);
}

bool queuedAudioSourceWrite(AudioState* state,
                            const float* interleaved,
                            uint64_t frames,
                            int64_t ptsUs,
                            int serial,
                            bool allowBlock,
                            bool allowCommitInProgressWrite,
                            uint64_t* writtenFrames) {
  return queuedAudioSourceWriteInternal(state, interleaved, frames, ptsUs,
                                        serial, allowBlock,
                                        allowCommitInProgressWrite,
                                        writtenFrames);
}

bool queuedAudioSourceStartDecoderWorker(
    const AudioBackendHandlers* backend,
    uint64_t startFrame) {
  if (!queuedAudioSourceUsesDecoderWorker(backend) || !backend->read) {
    return false;
  }
  queuedAudioSourceStopDecoderWorker(&gAudio.state);
  gAudio.state.decodeStop.store(false, std::memory_order_relaxed);
  gAudio.state.decodeThreadRunning.store(true, std::memory_order_relaxed);
  const uint32_t workerChannels = gAudio.channels;
  const uint32_t workerRate = gAudio.sampleRate;
  const bool finishOnShortRead = backend->finishOnShortRead;
  gAudio.state.decodeThread = std::thread(
      [backend, startFrame, workerChannels, workerRate, finishOnShortRead]() {
        constexpr uint32_t kQueuedDecodeEmptyReadLimit = 32;
        std::vector<float> buffer(
            static_cast<size_t>(kRadioDspChunkFrames) * workerChannels);
        uint64_t framePos = startFrame;
        uint32_t consecutiveEmptyReads = 0;
        bool seekCommitPending = false;

        while (!gAudio.state.decodeStop.load(std::memory_order_relaxed)) {
          if (seekCommitPending &&
              !audioPipelineTransitionCommitInProgress(
                  gAudio.state.pipelineTransition)) {
            seekCommitPending = false;
          }

          if (!seekCommitPending && backend->seek &&
              gAudio.state.seekRequested.load(std::memory_order_relaxed) &&
              audioPipelineTransitionBeginCommit(
                  gAudio.state.pipelineTransition)) {
            int64_t target = gAudio.state.pendingSeekFrames.load(
                std::memory_order_relaxed);
            if (target < 0) target = 0;
            const uint64_t total =
                gAudio.state.totalFrames.load(std::memory_order_relaxed);
            if (total > 0 && static_cast<uint64_t>(target) > total) {
              target = static_cast<int64_t>(total);
            }
            uint64_t targetFrame = static_cast<uint64_t>(target);
            if (!backend->seek(targetFrame)) {
              targetFrame = 0;
              backend->seek(0);
            }
            framePos = targetFrame;
            consecutiveEmptyReads = 0;
            const uint64_t resetGeneration =
                queuedAudioSourceRequestSourceReset(&gAudio.state,
                                                    targetFrame);
            if (!queuedAudioSourceWaitForSourceReset(&gAudio.state,
                                                     resetGeneration)) {
              break;
            }
            seekCommitPending = transitionCommitIsCurrent(
                gAudio.state.pipelineTransition);
            continue;
          }

          if (!seekCommitPending &&
              audioPipelineTransitionActive(
                  gAudio.state.pipelineTransition)) {
            std::unique_lock<std::mutex> lock(
                gAudio.state.audioQueueMutex);
            gAudio.state.audioQueueCv.wait(lock, [&]() {
              return gAudio.state.decodeStop.load(
                         std::memory_order_relaxed) ||
                     !gAudio.state.streamQueueEnabled.load(
                         std::memory_order_relaxed) ||
                     !audioPipelineTransitionActive(
                         gAudio.state.pipelineTransition) ||
                     (gAudio.state.seekRequested.load(
                          std::memory_order_relaxed) &&
                      gAudio.state.pipelineTransition.phase.load(
                          std::memory_order_acquire) ==
                          AudioPipelineTransitionPhase::CommitReady);
            });
            continue;
          }

          const uint64_t writable =
              gAudio.state.decodedAudio.writableFrames();
          if (writable == 0) {
            std::unique_lock<std::mutex> lock(
                gAudio.state.audioQueueMutex);
            gAudio.state.audioQueueCv.wait(lock, [&]() {
              return gAudio.state.decodeStop.load(
                         std::memory_order_relaxed) ||
                     !gAudio.state.streamQueueEnabled.load(
                         std::memory_order_relaxed) ||
                     audioPipelineTransitionActive(
                         gAudio.state.pipelineTransition) ||
                     gAudio.state.decodedAudio.writableFrames() > 0;
            });
            continue;
          }

          const uint32_t framesToRead = static_cast<uint32_t>(
              std::min<uint64_t>(writable, kRadioDspChunkFrames));
          uint64_t framesRead = 0;
          if (!backend->read(buffer.data(), framesToRead, &framesRead)) {
            gAudio.state.sourceAtEnd.store(true,
                                           std::memory_order_relaxed);
            gAudio.state.radioDspCv.notify_all();
            break;
          }

          bool reachedEnd = false;
          if (framesRead == 0) {
            if (queuedDecodeReachedKnownEnd(&gAudio.state, framePos)) {
              reachedEnd = true;
            } else if (++consecutiveEmptyReads >=
                       kQueuedDecodeEmptyReadLimit) {
              reachedEnd = true;
            } else {
              waitForQueuedDecodeRetry(&gAudio.state);
            }
          } else if (finishOnShortRead && framesRead < framesToRead &&
                     queuedDecodeReachedKnownEnd(&gAudio.state,
                                                framePos + framesRead)) {
            reachedEnd = true;
          }

          if (framesRead > 0) {
            consecutiveEmptyReads = 0;
            uint64_t remaining = framesRead;
            uint64_t offset = 0;
            while (remaining > 0 &&
                   !gAudio.state.decodeStop.load(
                       std::memory_order_relaxed)) {
              uint64_t written = 0;
              const int64_t ptsUs = static_cast<int64_t>(
                  (framePos + offset) * 1000000ULL / workerRate);
              if (!queuedAudioSourceWriteInternal(
                      &gAudio.state,
                      buffer.data() +
                          static_cast<size_t>(offset) * workerChannels,
                      remaining, ptsUs, 0, true, seekCommitPending,
                      &written)) {
                break;
              }
              if (written == 0) continue;
              remaining -= written;
              offset += written;
            }
            framePos += offset;
            if (queuedDecodeReachedKnownEnd(&gAudio.state, framePos)) {
              reachedEnd = true;
            }
          }

          if (reachedEnd) {
            gAudio.state.sourceAtEnd.store(true,
                                           std::memory_order_relaxed);
            gAudio.state.radioDspCv.notify_all();
            break;
          }
        }

        gAudio.state.decodeThreadRunning.store(false,
                                               std::memory_order_release);
        gAudio.state.audioQueueCv.notify_all();
      });
  return true;
}
