#include "audioplayback.h"
#include "app_common.h"

extern "C" {
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "clock.h"
#include "media_formats.h"
#include "ffmpegaudio.h"
#include "flacaudio.h"
#include "gmeaudio.h"
#include "gsfaudio.h"
#include "vgmaudio.h"
#include "kssaudio.h"
#include "melodyanalysiscache.h"
#include "midiaudio.h"
#include "psfaudio.h"
#include "radio.h"
#include "audiofilter/radio1938/preview/radio_preview_pipeline.h"
#include "pipeline_transition.h"
#include "output_volume_safety.h"
#include "playback_device.h"
#include "playback_source_priming.h"
#include "playback_sources/m4a_playback_source.h"
#include "runtime_helpers.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100 4189 4244 4245 4267 4456 4458 4996)
#endif
#define MINIAUDIO_IMPLEMENTATION
#define MA_ENABLE_WAV
#define MA_ENABLE_MP3
#define MA_ENABLE_FLAC
#include "miniaudio.h"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "timing_log.h"
#include "audioplayback_internal.h"
#include "miniaudio_file_path.h"

AudioPlaybackState gAudio;

void appendAudioTimingLogLine(const char* line) {
#if RADIOIFY_ENABLE_TIMING_LOG
  if (!line || line[0] == '\0') return;
  std::ofstream f(radioifyLogPath(), std::ios::app);
  if (f) f << radioifyLogTimestamp() << " " << line << "\n";
#else
  (void)line;
#endif
}

static constexpr uint32_t kRadioProcessChannels = 1u;
static bool backendUsesQueuedDecodeThread(const AudioBackendHandlers* backend);
static void clearQueuedPlaybackMetadata(AudioState* state);
static void resetQueuedPlaybackState(AudioState* state,
                                     uint64_t framePos,
                                     int serial);
static void stopDecodeWorker();
static bool writeQueuedSamples(AudioState* state,
                               float* interleaved,
                               uint64_t frames,
                               int64_t ptsUs,
                               int serial,
                               bool allowBlock,
                               uint64_t* writtenFrames);
static bool startDecodeWorker(const AudioBackendHandlers* backend,
                              uint64_t startFrame);

static void rebuildRadioFromTemplate(Radio1938* target,
                                     const Radio1938& source,
                                     float sampleRate,
                                     float bwHz,
                                     float noise) {
  if (!target) return;
  *target = source;
  target->init(kRadioProcessChannels, sampleRate, bwHz, noise);
}

static void applyRadioSettingsToTemplate(Radio1938& target,
                                        const std::string& presetName,
                                        const std::string& settingsPath) {
  if (settingsPath.empty()) return;
  std::string error;
  if (!applyRadioSettingsIni(target, settingsPath, presetName, &error)) {
    std::fprintf(stderr,
                 "WARNING: Failed to apply radio settings from %s: %s\n",
                 settingsPath.c_str(),
                 error.c_str());
  }
}

void rebuildRadioPreviewChain(AudioState* state) {
  if (!state) return;
  rebuildRadioFromTemplate(&state->radio1938, gAudio.radio1938Template,
                           static_cast<float>(gAudio.sampleRate), gAudio.lpHz,
                           gAudio.noise);
  state->radioPreviewConfig.programBandwidthHz = 0.48f * gAudio.lpHz;
  state->radioPreview.initialize(state->radio1938,
                                 state->radioAmIngress,
                                 state->radioPreviewConfig,
                                 static_cast<float>(gAudio.sampleRate));
}

void applyRadioTogglePreset() {
  gAudio.radio1938Template.applyPreset(Radio1938::Preset::Philco37116X);
  applyRadioSettingsToTemplate(gAudio.radio1938Template,
                               gAudio.radioPresetName,
                               gAudio.radioSettingsPath);
  rebuildRadioPreviewChain(&gAudio.state);
}

void stopAuditionWorker() {
  if (!gAudio.audition.active.load()) return;
  gAudio.audition.stop.store(true);
  if (gAudio.audition.worker.joinable()) {
    gAudio.audition.worker.join();
  }
  gAudio.audition.stop.store(false);
  gAudio.audition.active.store(false);
  gAudio.audition.device = KssInstrumentDevice::None;
  gAudio.audition.hash = 0;
}

void startAuditionWorker(AuditionTone tone) {
  gAudio.audition.stop.store(false);
  gAudio.audition.active.store(true);
  gAudio.audition.worker = std::thread([tone = std::move(tone)]() mutable {
    const uint32_t sampleRate = gAudio.sampleRate;
    const uint32_t channels = gAudio.channels;
    constexpr uint64_t kChunkFrames = 512;
    std::vector<float> buffer;
    buffer.resize(static_cast<size_t>(kChunkFrames) * channels);
    uint64_t framePos = 0;
    while (!gAudio.audition.stop.load()) {
      if (!gAudio.state.externalStream.load() ||
          !gAudio.state.streamQueueEnabled.load()) {
        break;
      }
      for (uint64_t i = 0; i < kChunkFrames; ++i) {
        float sample = renderAuditionSample(tone);
        for (uint32_t ch = 0; ch < channels; ++ch) {
          buffer[static_cast<size_t>(i * channels + ch)] = sample;
        }
      }
      uint64_t remaining = kChunkFrames;
      uint64_t offset = 0;
      while (remaining > 0 && !gAudio.audition.stop.load()) {
        int64_t ptsUs =
            static_cast<int64_t>((framePos + offset) * 1000000ULL / sampleRate);
        uint64_t written = 0;
        if (!audioStreamWriteSamples(
                buffer.data() + static_cast<size_t>(offset) * channels, remaining,
                ptsUs, 0, false, &written)) {
          remaining = 0;
          break;
        }
        if (written == 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
          continue;
        }
        remaining -= written;
        offset += written;
      }
      framePos += offset;
    }
    if (tone.kind == AuditionKind::Psg && tone.psg) {
      PSG_delete(tone.psg);
      tone.psg = nullptr;
    }
    if (tone.kind == AuditionKind::SccWave && tone.scc) {
      SCC_delete(tone.scc);
      tone.scc = nullptr;
    }
    gAudio.audition.active.store(false);
  });
}

static void updatePeakMeter(AudioState* state, const float* samples,
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

static void holdClipAlert(AudioState* state) {
  if (!state) return;
  constexpr int64_t kClipAlertHoldUs = 900000;
  state->clipAlertUntilUs.store(nowUs() + kClipAlertHoldUs,
                                std::memory_order_relaxed);
}

static void processRadioBlock(AudioState* state,
                              float* samples,
                              uint32_t frames) {
  if (!state || !samples || frames == 0) return;
  if (state->radioResetPending.exchange(false, std::memory_order_relaxed)) {
    rebuildRadioPreviewChain(state);
  }
  const uint32_t channels = std::max(1u, state->channels);
  audioPipelineTransitionApplyInputFadeIn(state->pipelineTransition, samples,
                                          frames, channels);
  state->radioPreview.runBlock(state->radio1938, samples, frames, channels);
  if (state->radio1938.diagnostics.anyClip) {
    holdClipAlert(state);
  }
}

bool audioPlaybackFinishSeekPipelineTransition(AudioState* state,
                                               bool resetRadio) {
  if (!state) return false;
  if (audioPipelineTransitionFinishCommit(state->pipelineTransition,
                                          state->sampleRate)) {
    state->seekRequested.store(false, std::memory_order_relaxed);
    if (resetRadio) {
      state->radioResetPending.store(true, std::memory_order_relaxed);
    }
    return true;
  }
  return false;
}

static bool commitPendingStreamSerialFlush(AudioState* state) {
  if (!state ||
      !state->streamSerialFlushPending.exchange(false,
                                                std::memory_order_relaxed)) {
    return false;
  }

  const int serial =
      state->pendingStreamSerial.exchange(0, std::memory_order_relaxed);
  state->streamSerial.store(serial, std::memory_order_relaxed);
  state->streamRb.reset();
  state->streamBaseValid.store(false, std::memory_order_relaxed);
  state->streamBasePtsUs.store(0, std::memory_order_relaxed);
  state->streamReadFrames.store(0, std::memory_order_relaxed);
  state->streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  state->sourceAtEnd.store(false, std::memory_order_relaxed);
  state->finished.store(false, std::memory_order_relaxed);
  state->streamClockReady.store(false, std::memory_order_relaxed);
  state->streamStarved.store(false, std::memory_order_relaxed);
  state->audioClock.reset(serial);
  clearQueuedPlaybackMetadata(state);
  state->radioResetPending.store(true, std::memory_order_relaxed);
  audioPipelineTransitionFinishCommit(state->pipelineTransition,
                                      state->sampleRate);
  state->decodeCv.notify_all();
  return true;
}

static std::chrono::steady_clock::duration playbackPipelineDrainBudget(
    const AudioState* state) {
  const uint32_t sampleRate = std::max(1u, state ? state->sampleRate : 48000u);
  const uint32_t transitionFrames = audioPipelineTransitionFrames(sampleRate);
  const uint32_t callbackFrames =
      state ? state->lastCallbackFrames.load(std::memory_order_relaxed) : 0;
  const uint64_t observedCallbackFrames =
      callbackFrames > 0 ? callbackFrames : transitionFrames;
  const uint64_t waitFrames =
      static_cast<uint64_t>(transitionFrames) + observedCallbackFrames * 4u;
  const uint64_t waitUs =
      (waitFrames * 1000000ull + sampleRate - 1u) / sampleRate;
  return std::chrono::microseconds(waitUs);
}

static void drainPlaybackPipelineForReplacement(AudioState* state) {
  if (!state || !gAudio.deviceReady ||
      !state->audioPrimed.load(std::memory_order_relaxed)) {
    return;
  }
  if (state->paused.load(std::memory_order_relaxed) ||
      state->hold.load(std::memory_order_relaxed)) {
    return;
  }

  audioPipelineTransitionRequestDiscontinuity(state->pipelineTransition,
                                              state->sampleRate);
  state->decodeCv.notify_all();
  state->m4aCv.notify_all();

  const auto deadline =
      std::chrono::steady_clock::now() + playbackPipelineDrainBudget(state);
  while (std::chrono::steady_clock::now() < deadline) {
    if (audioPipelineTransitionBeginCommit(state->pipelineTransition)) {
      return;
    }
    if (!audioPipelineTransitionActive(state->pipelineTransition)) {
      return;
    }
    std::unique_lock<std::mutex> lock(state->decodeMutex);
    state->decodeCv.wait_until(lock, deadline);
  }
  audioPipelineTransitionBeginCommit(state->pipelineTransition);
}

static bool backendUsesQueuedDecodeThread(const AudioBackendHandlers* backend) {
  return backend && backend->mode != AudioMode::None &&
         backend->mode != AudioMode::Stream &&
         backend->mode != AudioMode::M4a;
}

static void clearQueuedPlaybackMetadata(AudioState* state) {
  if (!state) return;
  std::lock_guard<std::mutex> lock(state->streamMetadataMutex);
  state->streamMetadata.clear();
}

static void resetQueuedPlaybackState(AudioState* state,
                                     uint64_t framePos,
                                     int serial) {
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
  state->streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  state->audioClock.reset(serial);
  audioPipelineTransitionRequestSignalFadeIn(state->pipelineTransition,
                                             state->sampleRate);
  clearQueuedPlaybackMetadata(state);
  state->decodeCv.notify_all();
}

static void stopDecodeWorker() {
  gAudio.state.decodeStop.store(true, std::memory_order_relaxed);
  gAudio.state.decodeCv.notify_all();
  if (gAudio.state.decodeThread.joinable()) {
    gAudio.state.decodeThread.join();
  }
  gAudio.state.decodeThreadRunning.store(false, std::memory_order_relaxed);
  gAudio.state.decodeStop.store(false, std::memory_order_relaxed);
}

static bool queuedDecodeReachedKnownEnd(const AudioState* state,
                                        uint64_t framePos) {
  if (!state) return false;
  uint64_t totalFrames = state->totalFrames.load(std::memory_order_relaxed);
  return totalFrames > 0 && framePos >= totalFrames;
}

static int64_t queuedAudioClockStarvationGraceUs(const AudioState* state,
                                                 uint32_t frameCount) {
  constexpr int64_t kMinClockStarvationGraceUs = 150000;
  constexpr int64_t kMaxClockStarvationGraceUs = 2000000;

  if (!state || state->sampleRate == 0) {
    return kMinClockStarvationGraceUs;
  }

  uint64_t bufferedFrames = state->deviceDelayFrames.load(std::memory_order_relaxed);
  if (frameCount > 0) {
    bufferedFrames =
        std::max<uint64_t>(bufferedFrames, static_cast<uint64_t>(frameCount) * 2ULL);
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

static bool waitForQueuedDecodeRetry(AudioState* state) {
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

static bool waitForQueuedPlaybackSourcePrimed(AudioState* state,
                                              uint32_t primeFrames) {
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

static bool writeQueuedSamples(AudioState* state,
                               float* interleaved,
                               uint64_t frames,
                               int64_t ptsUs,
                               int serial,
                               bool allowBlock,
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
          {state->streamRb.wpos.load(std::memory_order_relaxed), ptsUs, serial});
    }
  }

  uint64_t remaining = frames;
  float* cursor = interleaved;
  uint64_t totalWritten = 0;
  while (remaining > 0) {
    if (!state->externalStream.load(std::memory_order_relaxed) &&
        audioPipelineTransitionActive(state->pipelineTransition)) {
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
               audioPipelineTransitionActive(state->pipelineTransition) ||
               !state->streamQueueEnabled.load(std::memory_order_relaxed) ||
               serial != state->streamSerial.load(std::memory_order_relaxed) ||
               state->streamRb.writableFrames() > 0;
      });
      continue;
    }

    uint64_t framesToWrite = std::min(remaining, writable);
    if (!state->dry && state->useRadio1938.load()) {
      uint64_t radioOffset = 0;
      while (radioOffset < framesToWrite) {
        const uint32_t radioFrames = static_cast<uint32_t>(
            std::min<uint64_t>(framesToWrite - radioOffset, UINT32_MAX));
        processRadioBlock(
            state, cursor + static_cast<size_t>(radioOffset) * state->channels,
            radioFrames);
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

static bool startDecodeWorker(const AudioBackendHandlers* backend,
                              uint64_t startFrame) {
  if (!backendUsesQueuedDecodeThread(backend) || !backend->read) {
    return false;
  }
  stopDecodeWorker();
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
          if (!seekCommitPending && backend->seek &&
              gAudio.state.seekRequested.load(std::memory_order_relaxed) &&
              audioPipelineTransitionBeginCommit(
                  gAudio.state.pipelineTransition)) {
            int64_t target = gAudio.state.pendingSeekFrames.load(
                std::memory_order_relaxed);
            if (target < 0) target = 0;
            uint64_t total = gAudio.state.totalFrames.load(std::memory_order_relaxed);
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
            resetQueuedPlaybackState(&gAudio.state, targetFrame, 0);
            gAudio.state.radioResetPending.store(true,
                                                 std::memory_order_relaxed);
            seekCommitPending = true;
            continue;
          }

          if (!seekCommitPending &&
              audioPipelineTransitionActive(gAudio.state.pipelineTransition)) {
            std::unique_lock<std::mutex> lock(gAudio.state.decodeMutex);
            gAudio.state.decodeCv.wait_for(
                lock, std::chrono::milliseconds(5), [&]() {
                  return gAudio.state.decodeStop.load(std::memory_order_relaxed) ||
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
                  return gAudio.state.decodeStop.load(std::memory_order_relaxed) ||
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
              if (!writeQueuedSamples(
                      &gAudio.state,
                      buffer.data() +
                          static_cast<size_t>(offset) * workerChannels,
                      remaining, ptsUs, 0, true, &written)) {
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
        if (!commitPendingStreamSerialFlush(state)) {
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
      uint64_t currentRpos = state->streamRb.rpos.load(std::memory_order_relaxed);
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
              currentPts = md.ptsUs + static_cast<int64_t>(offset * 1000000ULL / state->sampleRate);
              currentSerial = md.serial;
          }
        }
      }

      if (currentPts != AV_NOPTS_VALUE) {
        // Compensate for hardware latency: the clock represents what is currently being heard.
        uint64_t delay = state->deviceDelayFrames.load(std::memory_order_relaxed);
        int64_t delayUs = static_cast<int64_t>(delay * 1000000ULL / state->sampleRate);
        
        state->audioClock.set(currentPts - delayUs, nowUs(), currentSerial);
        state->streamClockReady.store(true, std::memory_order_relaxed);
        
        state->streamUpdateCounter.fetch_add(1, std::memory_order_release);
        state->streamUpdateCv.notify_all();
      }
    }

    bool starved = (framesRead < frameCount);
    const bool wasStreamStarved =
        state->streamStarved.exchange(starved, std::memory_order_relaxed);
    
    // If we starved, check how long it's been since the last valid update.
    // High-latency outputs such as Remote Desktop can legitimately go much
    // longer between stable hardware updates than local speakers, so scale the
    // free-run window with the device buffer budget instead of using a hard
    // local-only threshold.
    if (starved) {
      audioPipelineTransitionRequestSignalFadeIn(state->pipelineTransition,
                                                 state->sampleRate);
      int64_t now = nowUs();
      int64_t last = state->audioClock.last_updated_us.load(std::memory_order_relaxed);
      int64_t graceUs = queuedAudioClockStarvationGraceUs(state, frameCount);
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
      if (!commitPendingStreamSerialFlush(state)) {
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
      holdClipAlert(state);
    }
    updatePeakMeter(state, out, frameCount);
    return;
  }

  if (state->hold.load()) {
    if (audioPipelineTransitionBeginFadeOut(state->pipelineTransition)) {
      if (!commitPendingStreamSerialFlush(state)) {
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
    state->framesRequested.store(static_cast<uint64_t>(target)); // Reset request count too
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
      if (!commitPendingStreamSerialFlush(state)) {
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
    audioPipelineTransitionRequestSignalFadeIn(state->pipelineTransition,
                                               state->sampleRate);
  }
  if (!state->dry && framesRead > 0 && state->useRadio1938.load()) {
    float* audioStart =
        out + silentLeadFrames * static_cast<uint64_t>(state->channels);
    processRadioBlock(state, audioStart, static_cast<uint32_t>(framesRead));
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
    if (!commitPendingStreamSerialFlush(state)) {
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
    holdClipAlert(state);
  }
  updatePeakMeter(state, out, frameCount);
}

void applyVgmDeviceOverrides() {
  if (gAudio.state.mode.load(std::memory_order_relaxed) != AudioMode::Vgm) {
    return;
  }
  if (gAudio.vgmDeviceOverrides.empty()) return;
  for (const auto& entry : gAudio.vgmDeviceOverrides) {
    gAudio.state.vgm.setDeviceOptions(entry.first, entry.second);
  }
}

const VgmDeviceInfo* findVgmDeviceInfo(uint32_t deviceId) {
  for (const auto& device : gAudio.vgmDevices) {
    if (device.id == deviceId) return &device;
  }
  return nullptr;
}

AudioMode currentAudioMode() {
  return gAudio.state.mode.load(std::memory_order_relaxed);
}

bool isAudioMode(AudioMode mode) {
  return currentAudioMode() == mode;
}

bool initFfmpegBackend(const std::filesystem::path& file, uint64_t, int,
                       std::string* error) {
  gAudio.state.totalFrames.store(0);
  return gAudio.state.ffmpeg.init(file, gAudio.channels, gAudio.sampleRate, error);
}

void uninitFfmpegBackend() { gAudio.state.ffmpeg.uninit(); }

bool readFfmpegBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.ffmpeg.readFrames(out, frameCount, framesRead);
}

bool seekFfmpegBackend(uint64_t frame) {
  return gAudio.state.ffmpeg.seekToFrame(frame);
}

bool totalFfmpegBackend(uint64_t* outFrames) {
  if (!outFrames) return false;
  return gAudio.state.ffmpeg.getTotalFrames(outFrames);
}

bool initFlacBackend(const std::filesystem::path& file, uint64_t, int,
                     std::string* error) {
  return gAudio.state.flac.init(file, gAudio.channels, gAudio.sampleRate, error);
}

void uninitFlacBackend() { gAudio.state.flac.uninit(); }

bool readFlacBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.flac.readFrames(out, frameCount, framesRead);
}

bool seekFlacBackend(uint64_t frame) { return gAudio.state.flac.seekToFrame(frame); }

bool totalFlacBackend(uint64_t* outFrames) {
  return gAudio.state.flac.getTotalFrames(outFrames);
}

bool initKssBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return gAudio.state.kss.init(file, gAudio.channels, gAudio.sampleRate, error,
                               trackIndex, gAudio.kssOptions);
}

void uninitKssBackend() { gAudio.state.kss.uninit(); }

bool readKssBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.kss.readFrames(out, frameCount, framesRead);
}

bool seekKssBackend(uint64_t frame) { return gAudio.state.kss.seekToFrame(frame); }

bool totalKssBackend(uint64_t* outFrames) {
  return gAudio.state.kss.getTotalFrames(outFrames);
}

bool initPsfBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return gAudio.state.psf.init(file, gAudio.channels, gAudio.sampleRate, error,
                               trackIndex);
}

void uninitPsfBackend() { gAudio.state.psf.uninit(); }

bool readPsfBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.psf.readFrames(out, frameCount, framesRead);
}

bool seekPsfBackend(uint64_t frame) { return gAudio.state.psf.seekToFrame(frame); }

bool totalPsfBackend(uint64_t* outFrames) {
  return gAudio.state.psf.getTotalFrames(outFrames);
}

bool initGsfBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return gAudio.state.gsf.init(file, gAudio.channels, gAudio.sampleRate, error,
                               trackIndex);
}

void uninitGsfBackend() { gAudio.state.gsf.uninit(); }

bool readGsfBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.gsf.readFrames(out, frameCount, framesRead);
}

bool seekGsfBackend(uint64_t frame) { return gAudio.state.gsf.seekToFrame(frame); }

bool totalGsfBackend(uint64_t* outFrames) {
  return gAudio.state.gsf.getTotalFrames(outFrames);
}

bool initVgmBackend(const std::filesystem::path& file, uint64_t, int,
                    std::string* error) {
  if (!gAudio.state.vgm.init(file, gAudio.channels, gAudio.sampleRate, error)) {
    return false;
  }
  gAudio.state.vgm.applyOptions(gAudio.vgmOptions);
  applyVgmDeviceOverrides();
  gAudio.vgmWarning = gAudio.state.vgm.warning();
  return true;
}

void uninitVgmBackend() { gAudio.state.vgm.uninit(); }

bool readVgmBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.vgm.readFrames(out, frameCount, framesRead);
}

bool seekVgmBackend(uint64_t frame) { return gAudio.state.vgm.seekToFrame(frame); }

bool totalVgmBackend(uint64_t* outFrames) {
  return gAudio.state.vgm.getTotalFrames(outFrames);
}

bool initGmeBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  if (!gAudio.state.gme.init(file, gAudio.channels, gAudio.sampleRate, error,
                             trackIndex)) {
    return false;
  }
  gAudio.state.gme.applyNsfOptions(gAudio.nsfOptions);
  gAudio.gmeWarning = gAudio.state.gme.warning();
  return true;
}

void uninitGmeBackend() { gAudio.state.gme.uninit(); }

bool readGmeBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.gme.readFrames(out, frameCount, framesRead);
}

bool seekGmeBackend(uint64_t frame) { return gAudio.state.gme.seekToFrame(frame); }

bool totalGmeBackend(uint64_t* outFrames) {
  return gAudio.state.gme.getTotalFrames(outFrames);
}

bool initMidiBackend(const std::filesystem::path& file, uint64_t, int,
                     std::string* error) {
  return gAudio.state.midi.init(file, gAudio.channels, gAudio.sampleRate, error);
}

void uninitMidiBackend() { gAudio.state.midi.uninit(); }

bool readMidiBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.midi.readFrames(out, frameCount, framesRead);
}

bool seekMidiBackend(uint64_t frame) { return gAudio.state.midi.seekToFrame(frame); }

bool totalMidiBackend(uint64_t* outFrames) {
  return gAudio.state.midi.getTotalFrames(outFrames);
}

bool initMiniaudioBackend(const std::filesystem::path& file, uint64_t, int,
                          std::string*) {
  ma_decoder_config decConfig =
      ma_decoder_config_init(ma_format_f32, gAudio.channels, gAudio.sampleRate);
  return maDecoderInitFilePath(file, &decConfig, &gAudio.state.decoder) ==
         MA_SUCCESS;
}

void uninitMiniaudioBackend() { ma_decoder_uninit(&gAudio.state.decoder); }

bool readMiniaudioBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  ma_uint64 read = 0;
  ma_result result =
      ma_decoder_read_pcm_frames(&gAudio.state.decoder, out, frameCount, &read);
  if (framesRead) *framesRead = static_cast<uint64_t>(read);
  return result == MA_SUCCESS || result == MA_AT_END;
}

bool seekMiniaudioBackend(uint64_t frame) {
  return ma_decoder_seek_to_pcm_frame(&gAudio.state.decoder,
                                      static_cast<ma_uint64>(frame)) ==
         MA_SUCCESS;
}

bool totalMiniaudioBackend(uint64_t* outFrames) {
  if (!outFrames) return false;
  ma_uint64 total = 0;
  if (ma_decoder_get_length_in_pcm_frames(&gAudio.state.decoder, &total) !=
      MA_SUCCESS) {
    return false;
  }
  *outFrames = static_cast<uint64_t>(total);
  return true;
}

std::string warningGmeBackend() { return gAudio.gmeWarning; }
std::string warningGsfBackend() { return gAudio.gsfWarning; }
std::string warningVgmBackend() { return gAudio.vgmWarning; }

const AudioBackendHandlers kBackendM4a{
    AudioMode::M4a, false, false, false, true, initM4aBackend,
    uninitM4aBackend, readM4aBackend, nullptr, totalM4aBackend, nullptr};
const AudioBackendHandlers kBackendFfmpeg{
    AudioMode::Ffmpeg, false, true, true, true, initFfmpegBackend,
    uninitFfmpegBackend, readFfmpegBackend, seekFfmpegBackend,
    totalFfmpegBackend, nullptr};
const AudioBackendHandlers kBackendFlac{
    AudioMode::Flac, false, true, true, true, initFlacBackend,
    uninitFlacBackend, readFlacBackend, seekFlacBackend, totalFlacBackend,
    nullptr};
const AudioBackendHandlers kBackendKss{
    AudioMode::Kss, true, true, true, true, initKssBackend,
    uninitKssBackend, readKssBackend, seekKssBackend, totalKssBackend,
    nullptr};
const AudioBackendHandlers kBackendPsf{
    AudioMode::Psf, true, true, true, false, initPsfBackend,
    uninitPsfBackend, readPsfBackend, seekPsfBackend, totalPsfBackend,
    nullptr};
const AudioBackendHandlers kBackendGsf{
    AudioMode::Gsf, true, true, true, false, initGsfBackend,
    uninitGsfBackend, readGsfBackend, seekGsfBackend, totalGsfBackend,
    warningGsfBackend};
const AudioBackendHandlers kBackendVgm{
    AudioMode::Vgm, true, true, true, true, initVgmBackend,
    uninitVgmBackend, readVgmBackend, seekVgmBackend, totalVgmBackend,
    warningVgmBackend};
const AudioBackendHandlers kBackendGme{
    AudioMode::Gme, true, true, true, true, initGmeBackend,
    uninitGmeBackend, readGmeBackend, seekGmeBackend, totalGmeBackend,
    warningGmeBackend};
const AudioBackendHandlers kBackendMidi{
    AudioMode::Midi, false, true, true, true, initMidiBackend,
    uninitMidiBackend, readMidiBackend, seekMidiBackend, totalMidiBackend,
    nullptr};
const AudioBackendHandlers kBackendMiniaudio{
    AudioMode::Miniaudio, false, true, true, true, initMiniaudioBackend,
    uninitMiniaudioBackend, readMiniaudioBackend, seekMiniaudioBackend,
    totalMiniaudioBackend, nullptr};

struct BackendSelector {
  bool (*matches)(const std::filesystem::path& file);
  const AudioBackendHandlers* backend;
};

const BackendSelector kBackends[] = {
    {isM4aExt, &kBackendM4a},         {isOggExt, &kBackendFfmpeg},
    {isFlacExt, &kBackendFlac},
    {isMiniaudioExt, &kBackendMiniaudio}, {isGmeExt, &kBackendGme},
    {isMidiExt, &kBackendMidi},       {isGsfExt, &kBackendGsf},
    {isVgmExt, &kBackendVgm},         {isKssExt, &kBackendKss},
    {isPsfExt, &kBackendPsf},
};

const AudioBackendHandlers* selectAudioBackend(const std::filesystem::path& file) {
  for (const auto& entry : kBackends) {
    if (entry.matches(file)) return entry.backend;
  }
  return nullptr;
}

std::string warningForBackend(const AudioBackendHandlers* backend) {
  if (backend && backend->warning) {
    return backend->warning();
  }
  return {};
}

void activateBackend(const AudioBackendHandlers* backend, int trackIndex) {
  gAudio.decoderReady = true;
  gAudio.state.backend = backend;
  gAudio.state.mode.store(backend ? backend->mode : AudioMode::None,
                          std::memory_order_relaxed);
  gAudio.state.externalStream.store(false);
  gAudio.trackIndex = (backend && backend->supportsTrackIndex) ? trackIndex : 0;
}

void storeTotalFramesFromBackend(const AudioBackendHandlers* backend) {
  if (!backend || !backend->totalFrames) {
    return;
  }
  uint64_t total = 0;
  bool ok = backend->totalFrames(&total);
  gAudio.state.totalFrames.store(ok ? total : 0);
}

void stopAndUninitActiveDecoder() {
  stopDecodeWorker();
  if (gAudio.decoderReady) {
    const AudioBackendHandlers* backend = gAudio.state.backend;
    if (backend && backend->uninit) {
      backend->uninit();
    }
  }
  gAudio.decoderReady = false;
  gAudio.state.backend = nullptr;
  gAudio.state.mode.store(AudioMode::None, std::memory_order_relaxed);
  gAudio.state.externalStream.store(false);
  gAudio.state.streamQueueEnabled.store(false, std::memory_order_relaxed);
  gAudio.state.streamSerial.store(0, std::memory_order_relaxed);
  gAudio.state.pendingStreamSerial.store(0, std::memory_order_relaxed);
  gAudio.state.streamSerialFlushPending.store(false, std::memory_order_relaxed);
  gAudio.state.streamBaseValid.store(false, std::memory_order_relaxed);
  gAudio.state.streamBasePtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.streamRb.reset();
  gAudio.state.audioClock.reset(0);
  clearQueuedPlaybackMetadata(&gAudio.state);
  gAudio.state.decodeCv.notify_all();
}

bool openDecoderForBackend(const AudioBackendHandlers* backend,
                           const std::filesystem::path& file,
                           uint64_t startFrame,
                           int trackIndex) {
  if (!backend || !backend->init) return false;
  std::string error;
  if (!backend->init(file, startFrame, trackIndex, &error)) {
    gAudio.lastInitError = error;
    if (backend->mode == AudioMode::Gsf) {
      gAudio.gsfWarning = error;
    } else if (backend->mode == AudioMode::Vgm) {
      gAudio.vgmWarning = error;
    } else {
      gAudio.gmeWarning = error;
    }
    return false;
  }
  gAudio.lastInitError.clear();
  storeTotalFramesFromBackend(backend);
  return true;
}

void uninitOpenedDecoder(const AudioBackendHandlers* backend) {
  if (backend && backend->uninit) {
    backend->uninit();
  }
}

void seekLoadedDecoderToStart(const AudioBackendHandlers* backend,
                              uint64_t* startFrame) {
  if (!backend || !startFrame || !backend->seek) return;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total > 0 && *startFrame > total) {
    *startFrame = total;
  }
  if (*startFrame == 0) return;
  if (!backend->seek(*startFrame)) {
    *startFrame = 0;
    backend->seek(0);
  }
}

static void releasePlaybackPipelineForNewSignal() {
  audioPipelineTransitionReset(gAudio.state.pipelineTransition);
  audioPipelineTransitionRequestSignalFadeIn(gAudio.state.pipelineTransition,
                                             gAudio.state.sampleRate);
}

void resetPlaybackStateForLoad(uint64_t startFrame,
                               bool releasePipelineTransition) {
  gAudio.state.framesPlayed.store(startFrame);
  gAudio.state.audioClockFrames.store(startFrame);
  gAudio.state.callbackCount.store(0);
  gAudio.state.framesRequested.store(0);
  gAudio.state.framesReadTotal.store(0);
  gAudio.state.shortReadCount.store(0);
  gAudio.state.silentFrames.store(0);
  gAudio.state.pausedCallbacks.store(0);
  gAudio.state.lastCallbackFrames.store(0);
  gAudio.state.lastFramesRead.store(0);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingSeekFrames.store(0);
  gAudio.state.finished.store(false);
  gAudio.state.paused.store(false);
  gAudio.state.audioPaddingFrames.store(0);
  gAudio.state.audioTrailingPaddingFrames.store(0);
  gAudio.state.audioLeadSilenceFrames.store(0);
  gAudio.state.streamQueueEnabled.store(false, std::memory_order_relaxed);
  gAudio.state.streamSerial.store(0, std::memory_order_relaxed);
  gAudio.state.pendingStreamSerial.store(0, std::memory_order_relaxed);
  gAudio.state.streamSerialFlushPending.store(false, std::memory_order_relaxed);
  gAudio.state.streamBaseValid.store(false, std::memory_order_relaxed);
  gAudio.state.streamBasePtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.deviceDelayFrames.store(0, std::memory_order_relaxed);
  gAudio.state.radioResetPending.store(false, std::memory_order_relaxed);
  gAudio.state.outputSafety = {};
  if (releasePipelineTransition) {
    releasePlaybackPipelineForNewSignal();
  }

  gAudio.state.channels = gAudio.channels;
  rebuildRadioPreviewChain(&gAudio.state);
}

bool loadFileAt(const std::filesystem::path& file, uint64_t startFrame,
                int trackIndex) {
  if (!validateSupportedAudioInputFile(file, &gAudio.lastInitError)) {
    return false;
  }
  melodyOfflineStop();
  gAudio.state.audioLeadSilenceFrames.store(0);
  if (gAudio.audition.active.load()) {
    stopAuditionWorker();
    gAudio.audition.resumeValid = false;
  }

  gAudio.lastInitError.clear();
  gAudio.gmeWarning.clear();
  gAudio.gsfWarning.clear();
  gAudio.vgmWarning.clear();

  const AudioBackendHandlers* backend = selectAudioBackend(file);
  if (!backend) {
    gAudio.lastInitError = "Unsupported audio format.";
    return false;
  }

  drainPlaybackPipelineForReplacement(&gAudio.state);
  gAudio.state.sourcePreparing.store(true, std::memory_order_release);
  stopAndUninitActiveDecoder();
  resetPlaybackStateForLoad(startFrame, true);

  if (!openDecoderForBackend(backend, file, startFrame, trackIndex)) {
    gAudio.state.sourcePreparing.store(false, std::memory_order_release);
    audioPipelineTransitionReset(gAudio.state.pipelineTransition);
    return false;
  }

  seekLoadedDecoderToStart(backend, &startFrame);
  if (backendUsesQueuedDecodeThread(backend)) {
    resetPlaybackStateForLoad(startFrame, false);
    const PlaybackSourcePriming priming =
        playbackSourcePrimingForRate(gAudio.sampleRate);
    gAudio.state.streamRb.init(priming.capacityFrames, gAudio.channels);
    gAudio.state.streamQueueEnabled.store(true, std::memory_order_relaxed);
    resetQueuedPlaybackState(&gAudio.state, startFrame, 0);
    if (!startDecodeWorker(backend, startFrame)) {
      uninitOpenedDecoder(backend);
      gAudio.lastInitError = "Failed to start audio source decoder.";
      gAudio.state.sourcePreparing.store(false, std::memory_order_release);
      audioPipelineTransitionReset(gAudio.state.pipelineTransition);
      return false;
    }
    if (!waitForQueuedPlaybackSourcePrimed(&gAudio.state,
                                           priming.primeFrames)) {
      stopDecodeWorker();
      uninitOpenedDecoder(backend);
      gAudio.lastInitError = "Failed to prime audio source.";
      gAudio.state.sourcePreparing.store(false, std::memory_order_release);
      audioPipelineTransitionReset(gAudio.state.pipelineTransition);
      return false;
    }
    activateBackend(backend, trackIndex);
    gAudio.state.sourcePreparing.store(false, std::memory_order_release);
    releasePlaybackPipelineForNewSignal();
  } else {
    if (backend->seek) {
      resetPlaybackStateForLoad(startFrame, true);
    }
    activateBackend(backend, trackIndex);
    gAudio.state.sourcePreparing.store(false, std::memory_order_release);
  }

  if (!audioPlaybackDeviceEnsureRunning()) {
    gAudio.state.sourcePreparing.store(false, std::memory_order_release);
    stopAndUninitActiveDecoder();
    audioPipelineTransitionReset(gAudio.state.pipelineTransition);
    return false;
  }

  const uint64_t analysisLeadInFrames = gAudio.state.audioLeadSilenceFrames.load();
  if (backend->allowConcurrentOfflineAnalysis) {
    melodyOfflineStart(file, trackIndex, gAudio.sampleRate, gAudio.channels,
                      analysisLeadInFrames, gAudio.kssOptions,
                      gAudio.nsfOptions, gAudio.vgmOptions,
                      gAudio.vgmDeviceOverrides);
  }

  gAudio.nowPlaying = file;
  return true;
}

bool ensureChannels(uint32_t newChannels) {
  if (newChannels == gAudio.channels) return true;

  uint64_t resumeFrame = gAudio.decoderReady ? gAudio.state.framesPlayed.load()
                                             : 0;
  bool hadTrack = gAudio.decoderReady && !gAudio.nowPlaying.empty();
  int trackIndex = gAudio.trackIndex;

  drainPlaybackPipelineForReplacement(&gAudio.state);
  audioPlaybackDeviceUninit();
  if (gAudio.decoderReady) {
    stopAndUninitActiveDecoder();
  }

  gAudio.channels = newChannels;
  gAudio.state.channels = gAudio.channels;
  gAudio.radio1938Template.init(kRadioProcessChannels,
                                static_cast<float>(gAudio.sampleRate),
                                gAudio.lpHz,
                                gAudio.noise);
  rebuildRadioPreviewChain(&gAudio.state);

  if (hadTrack) {
    return loadFileAt(gAudio.nowPlaying, resumeFrame, trackIndex);
  }
  return true;
}

void stopPlayback() {
  if (gAudio.audition.active.load()) {
    stopAuditionWorker();
    gAudio.audition.resumeValid = false;
  }
  drainPlaybackPipelineForReplacement(&gAudio.state);
  audioPlaybackDeviceUninit();
  if (gAudio.decoderReady) {
    stopAndUninitActiveDecoder();
  }
  gAudio.state.framesPlayed.store(0);
  gAudio.state.audioClockFrames.store(0);
  gAudio.state.finished.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingSeekFrames.store(0);
  gAudio.state.radioResetPending.store(false, std::memory_order_relaxed);
  gAudio.state.sourcePreparing.store(false, std::memory_order_release);
  audioPipelineTransitionReset(gAudio.state.pipelineTransition);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.paused.store(false);
  gAudio.state.hold.store(false);
  gAudio.state.externalStream.store(false);
  gAudio.state.streamQueueEnabled.store(false);
  gAudio.state.streamSerial.store(0);
  gAudio.state.pendingStreamSerial.store(0);
  gAudio.state.streamSerialFlushPending.store(false);
  gAudio.state.streamBaseValid.store(false);
  gAudio.state.streamBasePtsUs.store(0);
  gAudio.state.streamReadFrames.store(0);
  gAudio.state.streamClockReady.store(false);
  gAudio.state.streamStarved.store(false);
  gAudio.state.streamRb.reset();
  gAudio.state.audioClock.reset(0);
  melodyOfflineStop();
  gAudio.nowPlaying.clear();
  gAudio.trackIndex = 0;
  gAudio.lastInitError.clear();
  gAudio.gmeWarning.clear();
  gAudio.gsfWarning.clear();
  gAudio.vgmWarning.clear();
}
void audioInit(const AudioPlaybackConfig& config) {
  gAudio.enableAudio = config.enableAudio;
  gAudio.enableRadio = config.enableRadio;
  gAudio.dry = config.dry;
  gAudio.sampleRate = 48000;
  gAudio.baseChannels = config.mono ? 1u : 2u;
  gAudio.channels = gAudio.baseChannels;
  gAudio.lpHz = static_cast<float>(config.bwHz);
  gAudio.noise = static_cast<float>(config.noise);
  gAudio.radioPresetName = config.radioPresetName;

  gAudio.state.channels = gAudio.channels;
  gAudio.state.sampleRate = gAudio.sampleRate;
  gAudio.state.dry = config.dry;
  gAudio.state.useRadio1938.store(config.enableRadio);
  gAudio.radioSettingsPath = config.radioSettingsPath;

  gAudio.radio1938Template.init(kRadioProcessChannels,
                                static_cast<float>(gAudio.sampleRate),
                                gAudio.lpHz,
                                gAudio.noise);
  if (config.enableRadio) {
    applyRadioTogglePreset();
  } else {
    rebuildRadioPreviewChain(&gAudio.state);
  }
}

void audioShutdown() {
  stopPlayback();
  audioPlaybackDeviceUninit();
}

bool audioIsEnabled() { return gAudio.enableAudio; }

bool audioIsReady() { return gAudio.decoderReady; }

bool audioStartFile(const std::filesystem::path& file, int trackIndex) {
  return loadFileAt(file, 0, trackIndex);
}

bool audioStartFileAt(const std::filesystem::path& file, double startSec,
                      int trackIndex) {
  if (!std::isfinite(startSec) || startSec <= 0.0) {
    return loadFileAt(file, 0, trackIndex);
  }
  double framesDouble = startSec * static_cast<double>(gAudio.sampleRate);
  int64_t startFrame = static_cast<int64_t>(std::llround(framesDouble));
  if (startFrame < 0) startFrame = 0;
  return loadFileAt(file, static_cast<uint64_t>(startFrame), trackIndex);
}

void audioStop() {
  gAudio.audition.resumeValid = false;
  stopPlayback();
}

bool audioStartStream(uint64_t totalFrames) {
  melodyOfflineStop();
  gAudio.lastInitError.clear();
  gAudio.gmeWarning.clear();
  gAudio.gsfWarning.clear();
  gAudio.vgmWarning.clear();
  gAudio.trackIndex = 0;
  drainPlaybackPipelineForReplacement(&gAudio.state);
  stopAndUninitActiveDecoder();

  const uint32_t rbFrames = std::max<uint32_t>(gAudio.sampleRate, 8192);
  gAudio.state.streamRb.init(rbFrames, gAudio.channels);
  gAudio.state.streamQueueEnabled.store(true);
  gAudio.state.streamSerial.store(0);
  gAudio.state.pendingStreamSerial.store(0);
  gAudio.state.streamSerialFlushPending.store(false);
  gAudio.state.streamBaseValid.store(false);
  gAudio.state.streamBasePtsUs.store(0);
  gAudio.state.streamReadFrames.store(0);
  gAudio.state.streamClockReady.store(false);
  gAudio.state.streamStarved.store(false);
  gAudio.state.audioClock.reset(0);
  gAudio.state.m4aThreadRunning.store(false);
  gAudio.state.m4aStop.store(false);
  gAudio.state.sourcePreparing.store(false, std::memory_order_release);
  gAudio.state.sourceAtEnd.store(false);
  gAudio.state.externalStream.store(true);
  gAudio.state.mode.store(AudioMode::Stream, std::memory_order_relaxed);
  gAudio.state.backend = nullptr;
  gAudio.decoderReady = true;
  gAudio.trackIndex = 0;

  gAudio.state.framesPlayed.store(0);
  gAudio.state.audioClockFrames.store(0);
  gAudio.state.callbackCount.store(0);
  gAudio.state.framesRequested.store(0);
  gAudio.state.framesReadTotal.store(0);
  gAudio.state.shortReadCount.store(0);
  gAudio.state.silentFrames.store(0);
  gAudio.state.pausedCallbacks.store(0);
  gAudio.state.lastCallbackFrames.store(0);
  gAudio.state.lastFramesRead.store(0);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingSeekFrames.store(0);
  gAudio.state.finished.store(false);
  gAudio.state.paused.store(false);
  gAudio.state.hold.store(false);
  gAudio.state.audioPaddingFrames.store(0);
  gAudio.state.audioTrailingPaddingFrames.store(0);
  gAudio.state.audioLeadSilenceFrames.store(0);
  gAudio.state.totalFrames.store(totalFrames);
  gAudio.state.radioResetPending.store(false, std::memory_order_relaxed);
  gAudio.state.outputSafety = {};
  audioPipelineTransitionReset(gAudio.state.pipelineTransition);
  audioPipelineTransitionRequestSignalFadeIn(gAudio.state.pipelineTransition,
                                             gAudio.state.sampleRate);

  gAudio.state.channels = gAudio.channels;
  gAudio.state.sampleRate = gAudio.sampleRate;
  rebuildRadioPreviewChain(&gAudio.state);

  if (!audioPlaybackDeviceEnsureRunning()) {
    stopAndUninitActiveDecoder();
    gAudio.state.streamQueueEnabled.store(false);
    audioPipelineTransitionReset(gAudio.state.pipelineTransition);
    return false;
  }
  audioPlaybackDeviceLatencyFrames();

  gAudio.nowPlaying.clear();
  return true;
}

void audioStopStream() {
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  stopPlayback();
}

size_t audioStreamBufferedFrames() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return static_cast<size_t>(gAudio.state.streamRb.bufferedFrames());
}

size_t audioStreamCapacityFrames() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return static_cast<size_t>(gAudio.state.streamRb.capFrames);
}

int64_t audioStreamOldestPtsUs() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return AV_NOPTS_VALUE;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return AV_NOPTS_VALUE;
  }

  // Gebruik de metadata wachtrij om de ECHTE tijdstempel van de oudste sample in de buffer te vinden.
  std::lock_guard<std::mutex> lock(gAudio.state.streamMetadataMutex);
  if (gAudio.state.streamMetadata.empty()) {
    return AV_NOPTS_VALUE;
  }

  const auto& md = gAudio.state.streamMetadata.front();
  uint64_t currentRpos = gAudio.state.streamRb.rpos.load(std::memory_order_relaxed);
  
  // Als we al samples hebben gelezen voorbij het startpunt van deze metadata
  if (currentRpos >= md.wpos) {
    uint64_t offset = currentRpos - md.wpos;
    return md.ptsUs + static_cast<int64_t>(offset * 1000000ULL / gAudio.state.sampleRate);
  } else {
    // Dit zou niet mogen gebeuren, maar voor de zekerheid:
    return md.ptsUs;
  }
}

bool audioStreamWriteSamples(float* interleaved, uint64_t frames,
                             int64_t ptsUs, int serial, bool allowBlock,
                             uint64_t* writtenFrames) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return false;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return false;
  }
  return writeQueuedSamples(&gAudio.state, interleaved, frames, ptsUs, serial,
                            allowBlock, writtenFrames);
}

void audioStreamPrimeClock(int serial, int64_t targetPtsUs) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load() ||
      !gAudio.state.streamQueueEnabled.load()) {
    return;
  }
  if (serial != gAudio.state.streamSerial.load(std::memory_order_relaxed)) {
    return;
  }

  // Seed the clock for priming without discarding buffered audio.
  // The callback will publish the real, latency-compensated clock once audio
  // hardware has started consuming samples.
  gAudio.state.audioClock.set(targetPtsUs, nowUs(), serial);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
}

void audioStreamDiscardUntil(int64_t ptsUs) {
  gAudio.state.streamDiscardPtsUs.store(ptsUs, std::memory_order_relaxed);
}

void audioStreamSetEnd(bool atEnd) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  gAudio.state.sourceAtEnd.store(atEnd);
}

void audioStreamReset(uint64_t framePos) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  gAudio.state.streamRb.reset();
  gAudio.state.framesPlayed.store(framePos);
  gAudio.state.audioClockFrames.store(framePos);
  gAudio.state.finished.store(false);
  gAudio.state.sourceAtEnd.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingStreamSerial.store(0, std::memory_order_relaxed);
  gAudio.state.streamSerialFlushPending.store(false, std::memory_order_relaxed);
  gAudio.state.pendingSeekFrames.store(static_cast<int64_t>(framePos));
  gAudio.state.audioPrimed.store(false);
  gAudio.state.streamBaseValid.store(false, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.audioClock.reset(
      gAudio.state.streamSerial.load(std::memory_order_relaxed));
  {
    std::lock_guard<std::mutex> lock(gAudio.state.streamMetadataMutex);
    gAudio.state.streamMetadata.clear();
  }
  gAudio.state.radioResetPending.store(true, std::memory_order_relaxed);
  audioPipelineTransitionRequestSignalFadeIn(gAudio.state.pipelineTransition,
                                             gAudio.state.sampleRate);
  gAudio.state.decodeCv.notify_all();
}

void audioStreamFlushSerial(int serial) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  gAudio.state.pendingStreamSerial.store(serial, std::memory_order_relaxed);
  gAudio.state.streamSerialFlushPending.store(true, std::memory_order_relaxed);
  audioPipelineTransitionRequestDiscontinuity(gAudio.state.pipelineTransition,
                                              gAudio.state.sampleRate);
  gAudio.state.decodeCv.notify_all();
}

int audioStreamSerial() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return gAudio.state.streamSerial.load(std::memory_order_relaxed);
}

int64_t audioStreamClockUs(int64_t nowUs) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  if (!gAudio.state.audioClock.is_valid()) {
    return 0;
  }
  
  // The audioClock is now updated in dataCallback with hardware latency compensation included.
  return gAudio.state.audioClock.get(nowUs);
}

int64_t audioStreamClockLastUpdatedUs() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return gAudio.state.audioClock.last_updated_us.load(std::memory_order_relaxed);
}

bool audioStreamStarved() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return false;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return false;
  }
  return gAudio.state.streamStarved.load(std::memory_order_relaxed);
}

bool audioStreamClockReady() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return false;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return false;
  }
  return gAudio.state.streamClockReady.load(std::memory_order_relaxed);
}

uint64_t audioStreamWaitForUpdate(uint64_t lastCounter, int timeoutMs) {
  std::unique_lock<std::mutex> lock(gAudio.state.streamMetadataMutex);
  gAudio.state.streamUpdateCv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&]() {
    return gAudio.state.streamUpdateCounter.load(std::memory_order_acquire) != lastCounter;
  });
  return gAudio.state.streamUpdateCounter.load(std::memory_order_acquire);
}

uint64_t audioStreamUpdateCounter() {
  return gAudio.state.streamUpdateCounter.load(std::memory_order_acquire);
}

static void requestAudioSeekFrame(int64_t target) {
  if (target < 0) target = 0;
  const uint64_t total = gAudio.state.totalFrames.load();
  if (total > 0 && target > static_cast<int64_t>(total)) {
    target = static_cast<int64_t>(total);
  }
  gAudio.state.pendingSeekFrames.store(target);
  gAudio.state.seekRequested.store(true);
  gAudio.state.finished.store(false);
  gAudio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.audioPrimed.store(false);
  audioPipelineTransitionRequestDiscontinuity(gAudio.state.pipelineTransition,
                                              gAudio.state.sampleRate);
  gAudio.state.m4aCv.notify_all();
  gAudio.state.decodeCv.notify_all();
}

std::filesystem::path audioGetNowPlaying() { return gAudio.nowPlaying; }

int audioGetTrackIndex() {
  if (!gAudio.decoderReady) return -1;
  const AudioBackendHandlers* backend = gAudio.state.backend;
  if (!backend || !backend->supportsTrackIndex) return -1;
  return gAudio.trackIndex;
}

double audioGetTimeSec() {
  if (!gAudio.decoderReady) {
    return 0.0;
  }
  if (!gAudio.state.audioPrimed.load()) {
    return 0.0;
  }
  int64_t frames = static_cast<int64_t>(gAudio.state.audioClockFrames.load());
  uint64_t latencyFrames = audioPlaybackDeviceLatencyFrames();
  frames -= static_cast<int64_t>(latencyFrames);
  if (frames < 0) {
    frames = 0;
  }
  double timeSec = static_cast<double>(frames) / gAudio.sampleRate;
  uint64_t totalFrames = gAudio.state.totalFrames.load();
  if (totalFrames > 0) {
    double totalSec = static_cast<double>(totalFrames) / gAudio.sampleRate;
    if (gAudio.state.finished.load() || timeSec > totalSec) {
      timeSec = totalSec;
    }
  }
  return timeSec;
}

double audioGetTotalSec() {
  if (!gAudio.decoderReady) {
    return -1.0;
  }
  uint64_t total = gAudio.state.totalFrames.load();
  if (total == 0) return -1.0;
  return static_cast<double>(total) / gAudio.sampleRate;
}

bool audioIsSeeking() {
  if (!gAudio.decoderReady) {
    return false;
  }
  return gAudio.state.seekRequested.load();
}

double audioGetSeekTargetSec() {
  if (!gAudio.decoderReady) {
    return -1.0;
  }
  uint64_t total = gAudio.state.totalFrames.load();
  if (total == 0) return -1.0;
  int64_t target = gAudio.state.pendingSeekFrames.load();
  if (target < 0) target = 0;
  if (static_cast<uint64_t>(target) > total) {
    target = static_cast<int64_t>(total);
  }
  return static_cast<double>(target) / gAudio.sampleRate;
}

bool audioIsPrimed() {
  if (!gAudio.decoderReady) return false;
  return gAudio.state.audioPrimed.load();
}

bool audioIsPaused() {
  if (!gAudio.decoderReady) return false;
  return gAudio.state.paused.load();
}

bool audioIsFinished() {
  if (!gAudio.decoderReady) return false;
  return gAudio.state.finished.load();
}

bool audioIsRadioEnabled() { return gAudio.state.useRadio1938.load(); }

bool audioIsHolding() { return gAudio.state.hold.load(); }

AudioPerfStats audioGetPerfStats() {
  AudioPerfStats stats{};
  if (!gAudio.decoderReady) {
    return stats;
  }
  stats.callbacks = gAudio.state.callbackCount.load(std::memory_order_relaxed);
  stats.framesRequested =
      gAudio.state.framesRequested.load(std::memory_order_relaxed);
  stats.framesRead = gAudio.state.framesReadTotal.load(std::memory_order_relaxed);
  stats.shortReads = gAudio.state.shortReadCount.load(std::memory_order_relaxed);
  stats.silentFrames = gAudio.state.silentFrames.load(std::memory_order_relaxed);
  stats.lastCallbackFrames =
      gAudio.state.lastCallbackFrames.load(std::memory_order_relaxed);
  stats.lastFramesRead =
      gAudio.state.lastFramesRead.load(std::memory_order_relaxed);
  stats.sampleRate = gAudio.state.sampleRate;
  stats.channels = gAudio.state.channels;
  const AudioMode mode = currentAudioMode();
  stats.usingFfmpeg = mode == AudioMode::M4a || mode == AudioMode::Ffmpeg;
  audioPlaybackDeviceFillPerfStats(&stats);
  return stats;
}

void audioTogglePause() {
  if (!gAudio.decoderReady) return;
  const bool wasPaused = gAudio.state.paused.load(std::memory_order_relaxed);
  if (!wasPaused) {
    gAudio.state.paused.store(true, std::memory_order_relaxed);
    return;
  }

  // Resume should reacquire the current output endpoint even when the backend
  // stayed logically started during a Windows default-device switch.
  if (!audioPlaybackDeviceRecreate()) {
    gAudio.lastInitError = "Failed to restore audio output device.";
    return;
  }

  gAudio.lastInitError.clear();
  gAudio.state.audioPrimed.store(false, std::memory_order_relaxed);
  gAudio.state.finished.store(false, std::memory_order_relaxed);
  gAudio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  if (gAudio.state.externalStream.load(std::memory_order_relaxed)) {
    gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
    gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
    gAudio.state.audioClock.reset(
        gAudio.state.streamSerial.load(std::memory_order_relaxed));
  }
  audioPipelineTransitionRequestOutputFadeIn(gAudio.state.pipelineTransition,
                                             gAudio.state.sampleRate);
  gAudio.state.paused.store(false, std::memory_order_relaxed);
}

void audioSeekBy(int direction) {
  if (!gAudio.decoderReady) return;
  int64_t deltaFrames = static_cast<int64_t>(direction) * 5 * gAudio.sampleRate;
  int64_t current = static_cast<int64_t>(gAudio.state.audioClockFrames.load());
  if (gAudio.state.seekRequested.load()) {
    current = gAudio.state.pendingSeekFrames.load();
  }
  requestAudioSeekFrame(current + deltaFrames);
}

void audioSeekToRatio(double ratio) {
  if (!gAudio.decoderReady) return;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total == 0) return;
  ratio = std::clamp(ratio, 0.0, 1.0);
  int64_t target = static_cast<int64_t>(ratio * static_cast<double>(total));
  requestAudioSeekFrame(target);
}

void audioSeekToSec(double sec) {
  if (!gAudio.decoderReady) return;
  if (!std::isfinite(sec)) return;
  int64_t target =
      static_cast<int64_t>(std::llround(sec * gAudio.sampleRate));
  requestAudioSeekFrame(target);
}

void audioToggleRadio() {
  bool next = !gAudio.state.useRadio1938.load();
  gAudio.state.useRadio1938.store(next);
  if (next) {
    applyRadioTogglePreset();
  }
  if (next) {
    audioPipelineTransitionRequestSignalFadeIn(gAudio.state.pipelineTransition,
                                               gAudio.state.sampleRate);
  } else {
    audioPipelineTransitionRequestOutputFadeIn(gAudio.state.pipelineTransition,
                                               gAudio.state.sampleRate);
  }
}

void audioSetHold(bool hold) {
  const bool wasHold = gAudio.state.hold.exchange(hold);
  if (wasHold && !hold) {
    audioPipelineTransitionRequestOutputFadeIn(gAudio.state.pipelineTransition,
                                               gAudio.state.sampleRate);
  }
}

void audioAdjustVolume(float delta) {
  float current = gAudio.state.volume.load(std::memory_order_relaxed);
  float next = std::clamp(current + delta, 0.0f, 4.0f);
  gAudio.state.volume.store(next, std::memory_order_relaxed);
}

float audioGetVolume() {
  return gAudio.state.volume.load(std::memory_order_relaxed);
}

float audioGetPeak() {
  return gAudio.state.peak.load(std::memory_order_relaxed);
}

bool audioHasClipAlert() {
  return gAudio.state.clipAlertUntilUs.load(std::memory_order_relaxed) >
         nowUs();
}

AudioMelodyInfo audioGetMelodyInfo() {
  if (!gAudio.decoderReady) {
    return AudioMelodyInfo{};
  }
  MelodyOfflineAnalysisState analysisState = melodyOfflineGetState();
  if (!analysisState.running && !analysisState.ready) {
    return {};
  }
  const MelodyOfflineFrame frame =
      melodyOfflineGetFrame(audioGetTimeSec());
  AudioMelodyInfo info{};
  info.frequencyHz = frame.frequencyHz;
  info.confidence = std::clamp(frame.confidence, 0.0f, 1.0f);
  info.midiNote = frame.midiNote;
  if (!std::isfinite(info.frequencyHz) || !std::isfinite(info.confidence) ||
      info.frequencyHz <= 0.0f || info.midiNote < 0 || info.midiNote > 127) {
    info.frequencyHz = 0.0f;
    info.midiNote = -1;
  }
  return info;
}

AudioMelodyAnalysisState audioGetMelodyAnalysisState() {
  const MelodyOfflineAnalysisState state = melodyOfflineGetState();
  AudioMelodyAnalysisState result;
  result.ready = state.ready;
  result.running = state.running;
  result.progress = state.progress;
  result.frameCount = state.frameCount;
  result.error = state.error;
  return result;
}

bool audioAnalyzeFileToMelodyFile(const std::filesystem::path& file,
                                  int trackIndex,
                                  const std::filesystem::path& outputFile,
                                  const std::function<void(float)>& progressCallback,
                                  std::string* error) {
  if (file.empty() || !std::filesystem::exists(file)) {
    if (error) *error = "Input file not found.";
    return false;
  }
  uint32_t analysisSampleRate = std::max<uint32_t>(1u, gAudio.sampleRate);
  uint32_t analysisChannels =
      std::clamp<uint32_t>(std::max<uint32_t>(1u, gAudio.baseChannels), 1u, 2u);
  return melodyOfflineAnalyzeToFile(
      file, trackIndex, analysisSampleRate, analysisChannels, 0,
      gAudio.kssOptions, gAudio.nsfOptions, gAudio.vgmOptions,
      gAudio.vgmDeviceOverrides, outputFile, progressCallback, error);
}

std::string audioGetWarning() {
  std::string warning = warningForBackend(gAudio.state.backend);
  if (!warning.empty()) return warning;
return gAudio.lastInitError;
}
