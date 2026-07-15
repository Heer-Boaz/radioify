#include "queued_audio_source.h"

extern "C" {
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "audioplayback_internal.h"
#include "pipeline_transition.h"
#include "playback_source_priming.h"

void AudioSampleRing::init(uint64_t capacityFrames, uint32_t ch) {
  channels = ch;
  capFrames = capacityFrames;
  buf.assign(capFrames * channels, 0.0f);
  rpos.store(0, std::memory_order_relaxed);
  wpos.store(0, std::memory_order_relaxed);
}

void AudioSampleRing::reset() {
  rpos.store(0, std::memory_order_release);
  wpos.store(0, std::memory_order_release);
}

uint64_t AudioSampleRing::bufferedFrames() const {
  uint64_t w = wpos.load(std::memory_order_acquire);
  uint64_t r = rpos.load(std::memory_order_acquire);
  return w - r;
}

uint64_t AudioSampleRing::writableFrames() const {
  return capFrames - bufferedFrames();
}

uint64_t AudioSampleRing::writeSome(const float* in, uint64_t frames) {
  uint64_t w = wpos.load(std::memory_order_relaxed);
  uint64_t r = rpos.load(std::memory_order_acquire);
  uint64_t avail = capFrames - (w - r);
  if (avail == 0) return 0;
  uint64_t n = (frames < avail) ? frames : avail;
  uint64_t wi = w % capFrames;
  uint64_t first = std::min<uint64_t>(n, capFrames - wi);
  std::memcpy(buf.data() + wi * channels, in, first * channels * sizeof(float));
  if (n > first) {
    std::memcpy(buf.data(), in + first * channels,
                (n - first) * channels * sizeof(float));
  }
  wpos.store(w + n, std::memory_order_release);
  return n;
}

uint64_t AudioSampleRing::readSome(float* out, uint64_t frames) {
  uint64_t r = rpos.load(std::memory_order_relaxed);
  uint64_t w = wpos.load(std::memory_order_acquire);
  uint64_t avail = w - r;
  if (avail == 0) return 0;
  uint64_t n = (frames < avail) ? frames : avail;
  uint64_t ri = r % capFrames;
  uint64_t first = std::min<uint64_t>(n, capFrames - ri);
  std::memcpy(out, buf.data() + ri * channels,
              first * channels * sizeof(float));
  if (n > first) {
    std::memcpy(out + first * channels, buf.data(),
                (n - first) * channels * sizeof(float));
  }
  rpos.store(r + n, std::memory_order_release);
  return n;
}

namespace {

bool queuedDecodeReachedKnownEnd(const AudioState* state, uint64_t framePos) {
  if (!state) return false;
  uint64_t totalFrames = state->totalFrames.load(std::memory_order_relaxed);
  return totalFrames > 0 && framePos >= totalFrames;
}

bool waitForQueuedDecodeRetry(AudioState* state) {
  if (!state) return false;
  std::unique_lock<std::mutex> lock(state->decodeMutex);
  state->decodeCv.wait_for(lock, std::chrono::milliseconds(5), [&]() {
    return state->decodeStop.load(std::memory_order_relaxed) ||
           audioPipelineTransitionActive(state->pipelineTransition) ||
           !state->streamQueueEnabled.load(std::memory_order_relaxed);
  });
  return !state->decodeStop.load(std::memory_order_relaxed) &&
         !audioPipelineTransitionActive(state->pipelineTransition) &&
         state->streamQueueEnabled.load(std::memory_order_relaxed);
}

bool queuedSourceWriteBlockedByTransition(
    const AudioState* state,
    bool allowCommitInProgressWrite) {
  if (!state || state->externalStream.load(std::memory_order_relaxed)) {
    return false;
  }
  if (!audioPipelineTransitionActive(state->pipelineTransition)) {
    return false;
  }
  return !allowCommitInProgressWrite ||
         !audioPipelineTransitionCommitInProgress(state->pipelineTransition);
}

bool queuedAudioSourceWriteInternal(AudioState* state,
                                    float* interleaved,
                                    uint64_t frames,
                                    int64_t ptsUs,
                                    int serial,
                                    bool allowBlock,
                                    bool allowCommitInProgressWrite,
                                    uint64_t* writtenFrames) {
  if (writtenFrames) {
    *writtenFrames = 0;
  }
  if (!state || !state->streamQueueEnabled.load(std::memory_order_relaxed) ||
      !interleaved || frames == 0) {
    return false;
  }
  if (serial != state->streamSerial.load(std::memory_order_relaxed)) {
    return true;
  }

  int64_t discardThreshold =
      state->streamDiscardPtsUs.load(std::memory_order_relaxed);
  if (discardThreshold > 0 && ptsUs < discardThreshold) {
    if (writtenFrames) *writtenFrames = frames;
    return true;
  } else if (discardThreshold > 0 && ptsUs >= discardThreshold) {
    state->streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  }

  if (!state->streamBaseValid.load(std::memory_order_relaxed)) {
    state->streamBasePtsUs.store(ptsUs, std::memory_order_relaxed);
    state->streamBaseValid.store(true, std::memory_order_relaxed);
    state->streamReadFrames.store(0, std::memory_order_relaxed);
    state->streamClockReady.store(false, std::memory_order_relaxed);
  }

  if (ptsUs != AV_NOPTS_VALUE) {
    std::lock_guard<std::mutex> lock(state->streamMetadataMutex);
    if (state->streamMetadata.empty() ||
        state->streamMetadata.back().ptsUs != ptsUs ||
        state->streamMetadata.back().serial != serial) {
      state->streamMetadata.push_back(
          {state->streamRb.wpos.load(std::memory_order_relaxed), ptsUs,
           serial});
    }
  }

  uint64_t remaining = frames;
  float* cursor = interleaved;
  uint64_t totalWritten = 0;
  while (remaining > 0) {
    if (queuedSourceWriteBlockedByTransition(state,
                                             allowCommitInProgressWrite)) {
      return false;
    }
    if (serial != state->streamSerial.load(std::memory_order_relaxed)) {
      return true;
    }
    if (!state->streamQueueEnabled.load(std::memory_order_relaxed)) {
      return false;
    }
    const uint64_t writable = state->streamRb.writableFrames();
    if (writable == 0) {
      if (!allowBlock) {
        break;
      }
      std::unique_lock<std::mutex> lock(state->decodeMutex);
      state->decodeCv.wait_for(lock, std::chrono::milliseconds(5), [&]() {
        return state->decodeStop.load(std::memory_order_relaxed) ||
               queuedSourceWriteBlockedByTransition(
                   state, allowCommitInProgressWrite) ||
               !state->streamQueueEnabled.load(std::memory_order_relaxed) ||
               serial != state->streamSerial.load(std::memory_order_relaxed) ||
               state->streamRb.writableFrames() > 0;
      });
      continue;
    }

    uint64_t framesToWrite = std::min(remaining, writable);
    if (!state->dry) {
      uint64_t radioOffset = 0;
      while (radioOffset < framesToWrite) {
        const uint32_t radioFrames = static_cast<uint32_t>(
            std::min<uint64_t>(framesToWrite - radioOffset, UINT32_MAX));
        if (state->radioFilter.process(
                cursor + static_cast<size_t>(radioOffset) * state->channels,
                radioFrames, state->channels, state->pipelineTransition)) {
          audioPlaybackHoldClipAlert(state);
        }
        radioOffset += radioFrames;
      }
    }

    uint64_t written = state->streamRb.writeSome(cursor, framesToWrite);
    if (written == 0) {
      continue;
    }
    remaining -= written;
    totalWritten += written;
    cursor += static_cast<size_t>(written) * state->channels;
  }
  if (writtenFrames) {
    *writtenFrames = totalWritten;
  }
  if (totalWritten > 0) {
    state->decodeCv.notify_all();
  }
  return true;
}

}  // namespace

bool queuedAudioSourceUsesDecodeThread(const AudioBackendHandlers* backend) {
  return backend && backend->mode != AudioMode::None &&
         backend->mode != AudioMode::Stream &&
         backend->mode != AudioMode::M4a;
}

void queuedAudioSourceClearMetadata(AudioState* state) {
  if (!state) return;
  std::lock_guard<std::mutex> lock(state->streamMetadataMutex);
  state->streamMetadata.clear();
}

void queuedAudioSourceReset(AudioState* state, uint64_t framePos, int serial) {
  if (!state) return;
  state->streamRb.reset();
  state->framesPlayed.store(framePos, std::memory_order_relaxed);
  state->audioClockFrames.store(framePos, std::memory_order_relaxed);
  state->finished.store(false, std::memory_order_relaxed);
  state->sourceAtEnd.store(false, std::memory_order_relaxed);
  state->audioPrimed.store(false, std::memory_order_relaxed);
  state->streamBaseValid.store(false, std::memory_order_relaxed);
  state->streamBasePtsUs.store(0, std::memory_order_relaxed);
  state->streamReadFrames.store(0, std::memory_order_relaxed);
  state->streamClockReady.store(false, std::memory_order_relaxed);
  state->streamStarved.store(false, std::memory_order_relaxed);
  state->pendingStreamDiscardPtsUs.store(0, std::memory_order_relaxed);
  state->streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  state->audioClock.reset(serial);
  audioPipelineTransitionRequestSignalFadeIn(state->pipelineTransition,
                                             state->sampleRate);
  queuedAudioSourceClearMetadata(state);
  state->decodeCv.notify_all();
}

void queuedAudioSourceStopWorker(AudioState* state) {
  if (!state) return;
  state->decodeStop.store(true, std::memory_order_relaxed);
  state->decodeCv.notify_all();
  if (state->decodeThread.joinable()) {
    state->decodeThread.join();
  }
  state->decodeThreadRunning.store(false, std::memory_order_relaxed);
  state->decodeStop.store(false, std::memory_order_relaxed);
}

bool queuedAudioSourceWaitPrimed(AudioState* state, uint32_t primeFrames) {
  if (!state) return false;
  std::unique_lock<std::mutex> lock(state->decodeMutex);
  state->decodeCv.wait(lock, [&]() {
    return playbackSourceIsPrimed(
               state->streamRb.bufferedFrames(), primeFrames,
               state->sourceAtEnd.load(std::memory_order_relaxed)) ||
           !state->decodeThreadRunning.load(std::memory_order_relaxed) ||
           state->decodeStop.load(std::memory_order_relaxed) ||
           !state->streamQueueEnabled.load(std::memory_order_relaxed);
  });

  return playbackSourceIsPrimed(
      state->streamRb.bufferedFrames(), primeFrames,
      state->sourceAtEnd.load(std::memory_order_relaxed));
}

bool queuedAudioSourceCommitPendingSerialFlush(AudioState* state) {
  if (!state ||
      !state->streamSerialFlushPending.exchange(false,
                                                std::memory_order_relaxed)) {
    return false;
  }

  const int serial =
      state->pendingStreamSerial.exchange(0, std::memory_order_relaxed);
  const int64_t discardUntilUs =
      state->pendingStreamDiscardPtsUs.exchange(0, std::memory_order_relaxed);
  state->streamSerial.store(serial, std::memory_order_relaxed);
  state->streamRb.reset();
  state->streamBaseValid.store(false, std::memory_order_relaxed);
  state->streamBasePtsUs.store(0, std::memory_order_relaxed);
  state->streamReadFrames.store(0, std::memory_order_relaxed);
  state->streamDiscardPtsUs.store((std::max)(int64_t{0}, discardUntilUs),
                                  std::memory_order_relaxed);
  state->sourceAtEnd.store(false, std::memory_order_relaxed);
  state->finished.store(false, std::memory_order_relaxed);
  state->streamClockReady.store(false, std::memory_order_relaxed);
  state->streamStarved.store(false, std::memory_order_relaxed);
  state->audioClock.reset(serial);
  queuedAudioSourceClearMetadata(state);
  state->radioFilter.requestReset();
  audioPipelineTransitionRequestSignalFadeIn(state->pipelineTransition,
                                             state->sampleRate);
  audioPipelineTransitionFinishCommit(state->pipelineTransition);
  state->decodeCv.notify_all();
  return true;
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
    bufferedFrames =
        std::max<uint64_t>(bufferedFrames,
                           static_cast<uint64_t>(frameCount) * 2ULL);
  }
  if (bufferedFrames == 0) {
    return kMinClockStarvationGraceUs;
  }

  int64_t bufferedUs = static_cast<int64_t>(
      (bufferedFrames * 1000000ULL) / state->sampleRate);
  if (bufferedUs <= 0) {
    return kMinClockStarvationGraceUs;
  }

  return std::clamp(bufferedUs * 2, kMinClockStarvationGraceUs,
                    kMaxClockStarvationGraceUs);
}

bool queuedAudioSourceWrite(AudioState* state,
                            float* interleaved,
                            uint64_t frames,
                            int64_t ptsUs,
                            int serial,
                            bool allowBlock,
                            uint64_t* writtenFrames) {
  return queuedAudioSourceWriteInternal(state, interleaved, frames, ptsUs, serial,
                                        allowBlock, false, writtenFrames);
}

bool queuedAudioSourceStartWorker(const AudioBackendHandlers* backend,
                                  uint64_t startFrame) {
  if (!queuedAudioSourceUsesDecodeThread(backend) || !backend->read) {
    return false;
  }
  queuedAudioSourceStopWorker(&gAudio.state);
  gAudio.state.decodeStop.store(false, std::memory_order_relaxed);
  gAudio.state.decodeThreadRunning.store(true, std::memory_order_relaxed);
  const uint32_t workerChannels = gAudio.channels;
  const uint32_t workerRate = gAudio.sampleRate;
  const uint32_t primeFrames =
      playbackSourcePrimingForRate(gAudio.sampleRate).primeFrames;
  const bool finishOnShortRead = backend->finishOnShortRead;
  gAudio.state.decodeThread =
      std::thread([backend, startFrame, workerChannels, workerRate,
                   primeFrames, finishOnShortRead]() {
        constexpr uint32_t kChunkFrames = 2048;
        constexpr uint32_t kQueuedDecodeEmptyReadLimit = 32;
        std::vector<float> buffer(static_cast<size_t>(kChunkFrames) *
                                  workerChannels);
        uint64_t framePos = startFrame;
        uint32_t consecutiveEmptyReads = 0;
        bool seekCommitPending = false;
        auto finishSeekCommitWhenPrimed = [&]() {
          if (!seekCommitPending) return;
          if (!playbackSourceIsPrimed(
                  gAudio.state.streamRb.bufferedFrames(), primeFrames,
                  gAudio.state.sourceAtEnd.load(std::memory_order_relaxed))) {
            return;
          }
          audioPlaybackFinishSeekPipelineTransition(&gAudio.state, false);
          seekCommitPending = false;
        };
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
            uint64_t total = gAudio.state.totalFrames.load(
                std::memory_order_relaxed);
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
            queuedAudioSourceReset(&gAudio.state, targetFrame, 0);
            gAudio.state.radioFilter.requestReset();
            seekCommitPending = true;
            continue;
          }

          if (!seekCommitPending &&
              audioPipelineTransitionActive(gAudio.state.pipelineTransition)) {
            std::unique_lock<std::mutex> lock(gAudio.state.decodeMutex);
            gAudio.state.decodeCv.wait_for(
                lock, std::chrono::milliseconds(5), [&]() {
                  return gAudio.state.decodeStop.load(
                             std::memory_order_relaxed) ||
                         !audioPipelineTransitionActive(
                             gAudio.state.pipelineTransition) ||
                         !gAudio.state.streamQueueEnabled.load(
                             std::memory_order_relaxed);
                });
            continue;
          }

          uint64_t writable = gAudio.state.streamRb.writableFrames();
          if (writable == 0) {
            std::unique_lock<std::mutex> lock(gAudio.state.decodeMutex);
            gAudio.state.decodeCv.wait_for(
                lock, std::chrono::milliseconds(5), [&]() {
                  return gAudio.state.decodeStop.load(
                             std::memory_order_relaxed) ||
                         audioPipelineTransitionActive(
                             gAudio.state.pipelineTransition) ||
                         !gAudio.state.streamQueueEnabled.load(
                             std::memory_order_relaxed) ||
                         gAudio.state.streamRb.writableFrames() > 0;
                });
            continue;
          }

          uint32_t framesToRead =
              static_cast<uint32_t>(std::min<uint64_t>(writable, kChunkFrames));
          uint64_t framesRead = 0;
          if (!backend->read(buffer.data(), framesToRead, &framesRead)) {
            gAudio.state.sourceAtEnd.store(true, std::memory_order_relaxed);
            finishSeekCommitWhenPrimed();
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
                   !gAudio.state.decodeStop.load(std::memory_order_relaxed)) {
              uint64_t written = 0;
              int64_t ptsUs = static_cast<int64_t>(
                  (framePos + offset) * 1000000ULL / workerRate);
              if (!queuedAudioSourceWriteInternal(
                      &gAudio.state,
                      buffer.data() +
                          static_cast<size_t>(offset) * workerChannels,
                      remaining, ptsUs, 0, true, seekCommitPending, &written)) {
                remaining = 0;
                break;
              }
              if (written == 0) {
                continue;
              }
              remaining -= written;
              offset += written;
            }
            framePos += offset;
            if (queuedDecodeReachedKnownEnd(&gAudio.state, framePos)) {
              reachedEnd = true;
            }
          }

          if (reachedEnd) {
            gAudio.state.sourceAtEnd.store(true, std::memory_order_relaxed);
            finishSeekCommitWhenPrimed();
            break;
          }
          finishSeekCommitWhenPrimed();
        }
        gAudio.state.decodeThreadRunning.store(false, std::memory_order_relaxed);
        gAudio.state.decodeCv.notify_all();
      });
  return true;
}
