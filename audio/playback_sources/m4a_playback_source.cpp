#include "m4a_playback_source.h"

#include "../audioplayback_internal.h"
#include "../queued_audio_source.h"
#include "runtime_helpers.h"
#include "timing_log.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

void stopM4aWorker() {
  AudioState& state = gAudio.state;
  state.m4aStop.store(true, std::memory_order_relaxed);
  state.audioQueueCv.notify_all();
  if (state.m4aThread.joinable()) {
    state.m4aThread.join();
  }
  state.m4aThreadRunning.store(false, std::memory_order_relaxed);
  state.m4aStop.store(false, std::memory_order_relaxed);
  state.sourceAtEnd.store(false, std::memory_order_relaxed);
  state.processedAtEnd.store(false, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(state.audioQueueMutex);
    state.m4aInitDone = false;
    state.m4aInitOk = false;
    state.m4aInitError.clear();
  }
}

bool startM4aWorker(const std::filesystem::path& file,
                    uint64_t startFrame,
                    std::string* error) {
  stopM4aWorker();

  AudioState& state = gAudio.state;
  {
    std::lock_guard<std::mutex> lock(state.audioQueueMutex);
    state.m4aInitDone = false;
    state.m4aInitOk = false;
    state.m4aInitError.clear();
  }
  state.m4aStop.store(false, std::memory_order_relaxed);
  state.sourceAtEnd.store(false, std::memory_order_relaxed);
  state.processedAtEnd.store(false, std::memory_order_relaxed);
  state.m4aThreadRunning.store(true, std::memory_order_release);

  const uint32_t workerChannels = gAudio.channels;
  const uint32_t workerRate = gAudio.sampleRate;
  state.m4aThread =
      std::thread([file, startFrame, workerChannels, workerRate]() {
        AudioState& state = gAudio.state;
        FfmpegAudioDecoder decoder;
        std::string initError;
#if RADIOIFY_ENABLE_TIMING_LOG
        const auto initStart = std::chrono::steady_clock::now();
#endif
        const bool ok =
            decoder.init(file, workerChannels, workerRate, &initError);
#if RADIOIFY_ENABLE_TIMING_LOG
        const auto initEnd = std::chrono::steady_clock::now();
        {
          char buf[512];
          std::snprintf(
              buf, sizeof(buf),
              "ffmpeg_audio_init_ms=%lld file=%s ok=%d err=%s",
              static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      initEnd - initStart)
                      .count()),
              toUtf8String(file.filename()).c_str(), ok ? 1 : 0,
              initError.empty() ? "-" : initError.c_str());
          appendAudioTimingLogLine(buf);
        }
#endif

        uint64_t total = 0;
        uint64_t paddingFrames = 0;
        uint64_t trailingFrames = 0;
        int64_t startOffsetFrames = 0;
        uint64_t leadSilenceFrames = 0;
        uint64_t localStart = startFrame;
        uint64_t rawTotalFrames = 0;
        if (ok) {
          decoder.getPaddingFrames(&paddingFrames, &trailingFrames);
          decoder.getStartOffsetFrames(&startOffsetFrames);
          decoder.getTotalFrames(&total);
          if (startOffsetFrames > 0) {
            leadSilenceFrames = static_cast<uint64_t>(startOffsetFrames);
          } else if (startOffsetFrames < 0) {
            const uint64_t extraSkip =
                static_cast<uint64_t>(-startOffsetFrames);
            paddingFrames += extraSkip;
            total = total > extraSkip ? total - extraSkip : 0;
          }
          if (total > 0 && localStart > total) localStart = total;
          if (total > 0) {
            rawTotalFrames = total + paddingFrames + trailingFrames;
          }
          uint64_t rawStart = localStart + paddingFrames;
          if (rawTotalFrames > 0 && rawStart > rawTotalFrames) {
            rawStart = rawTotalFrames;
          }
          if (rawStart > 0 && !decoder.seekToFrame(rawStart)) {
            localStart = 0;
            decoder.seekToFrame(0);
          }
        }

        {
          std::lock_guard<std::mutex> lock(state.audioQueueMutex);
          state.m4aInitDone = true;
          state.m4aInitOk = ok;
          state.m4aInitError = initError;
        }
        state.totalFrames.store(total, std::memory_order_relaxed);
        state.audioPaddingFrames.store(paddingFrames,
                                       std::memory_order_relaxed);
        state.audioTrailingPaddingFrames.store(trailingFrames,
                                               std::memory_order_relaxed);
        state.audioLeadSilenceFrames.store(leadSilenceFrames,
                                           std::memory_order_relaxed);
        state.framesPlayed.store(localStart, std::memory_order_relaxed);
        state.audioClockFrames.store(localStart, std::memory_order_relaxed);
        state.audioQueueCv.notify_all();
        if (!ok) {
          state.m4aThreadRunning.store(false, std::memory_order_release);
          return;
        }

        constexpr uint32_t kChunkFrames = 2048;
        std::vector<float> buffer(
            static_cast<size_t>(kChunkFrames) * workerChannels);
        bool firstBufferLogged = false;
        bool seekCommitPending = false;
        uint64_t framePosition = localStart;

        while (!state.m4aStop.load(std::memory_order_relaxed)) {
          if (seekCommitPending &&
              !audioPipelineTransitionCommitInProgress(
                  state.pipelineTransition)) {
            seekCommitPending = false;
          }

          if (!seekCommitPending &&
              state.seekRequested.load(std::memory_order_relaxed) &&
              audioPipelineTransitionBeginCommit(
                  state.pipelineTransition)) {
            int64_t target =
                state.pendingSeekFrames.load(std::memory_order_relaxed);
            if (target < 0) target = 0;
            uint64_t seekTargetFrames = static_cast<uint64_t>(target);
            uint64_t rawTarget = seekTargetFrames + paddingFrames;
            if (rawTotalFrames > 0 && rawTarget > rawTotalFrames) {
              rawTarget = rawTotalFrames;
            }
            if (!decoder.seekToFrame(rawTarget)) {
              seekTargetFrames = 0;
              decoder.seekToFrame(0);
            }
            framePosition = seekTargetFrames;
            state.audioLeadSilenceFrames.store(0,
                                               std::memory_order_relaxed);
            const uint64_t resetGeneration =
                queuedAudioSourceRequestSourceReset(&state,
                                                    seekTargetFrames);
            if (!queuedAudioSourceWaitForSourceReset(&state,
                                                     resetGeneration)) {
              break;
            }
            seekCommitPending = audioPipelineTransitionCommitInProgress(
                state.pipelineTransition);
            continue;
          }

          if (!seekCommitPending &&
              audioPipelineTransitionActive(state.pipelineTransition)) {
            std::unique_lock<std::mutex> lock(state.audioQueueMutex);
            state.audioQueueCv.wait(lock, [&]() {
              return state.m4aStop.load(std::memory_order_relaxed) ||
                     !state.streamQueueEnabled.load(
                         std::memory_order_relaxed) ||
                     !audioPipelineTransitionActive(
                         state.pipelineTransition) ||
                     (state.seekRequested.load(std::memory_order_relaxed) &&
                      state.pipelineTransition.phase.load(
                          std::memory_order_acquire) ==
                          AudioPipelineTransitionPhase::CommitReady);
            });
            continue;
          }

          if (state.sourceAtEnd.load(std::memory_order_relaxed)) {
            std::unique_lock<std::mutex> lock(state.audioQueueMutex);
            state.audioQueueCv.wait(lock, [&]() {
              return state.m4aStop.load(std::memory_order_relaxed) ||
                     !state.streamQueueEnabled.load(
                         std::memory_order_relaxed) ||
                     state.seekRequested.load(std::memory_order_relaxed) ||
                     !state.sourceAtEnd.load(std::memory_order_relaxed);
            });
            continue;
          }

          const uint64_t writable = state.decodedAudio.writableFrames();
          if (writable == 0) {
            std::unique_lock<std::mutex> lock(state.audioQueueMutex);
            state.audioQueueCv.wait(lock, [&]() {
              return state.m4aStop.load(std::memory_order_relaxed) ||
                     !state.streamQueueEnabled.load(
                         std::memory_order_relaxed) ||
                     audioPipelineTransitionActive(
                         state.pipelineTransition) ||
                     state.decodedAudio.writableFrames() > 0;
            });
            continue;
          }

          const uint32_t framesToRead = static_cast<uint32_t>(
              std::min<uint64_t>(writable, kChunkFrames));
          uint64_t framesRead = 0;
          if (!decoder.readFrames(buffer.data(), framesToRead,
                                  &framesRead) ||
              framesRead == 0) {
            state.sourceAtEnd.store(true, std::memory_order_relaxed);
            state.radioDspCv.notify_all();
            continue;
          }

          if (!firstBufferLogged) {
            firstBufferLogged = true;
#if RADIOIFY_ENABLE_TIMING_LOG
            const auto firstBufferTime = std::chrono::steady_clock::now();
            char buf[512];
            std::snprintf(
                buf, sizeof(buf),
                "ffmpeg_audio_first_buffer_ms=%lld frames=%llu file=%s",
                static_cast<long long>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        firstBufferTime - initStart)
                        .count()),
                static_cast<unsigned long long>(framesRead),
                toUtf8String(file.filename()).c_str());
            appendAudioTimingLogLine(buf);
#endif
          }

          uint64_t remaining = framesRead;
          uint64_t offset = 0;
          while (remaining > 0 &&
                 !state.m4aStop.load(std::memory_order_relaxed)) {
            const int64_t ptsUs = static_cast<int64_t>(
                (framePosition + offset) * 1000000ULL / workerRate);
            uint64_t written = 0;
            if (!queuedAudioSourceWrite(
                    &state,
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
          framePosition += offset;
        }

        state.m4aThreadRunning.store(false, std::memory_order_release);
        state.audioQueueCv.notify_all();
      });

  bool initOk = false;
  {
    std::unique_lock<std::mutex> lock(state.audioQueueMutex);
    state.audioQueueCv.wait(lock, [&]() {
      return state.m4aInitDone ||
             !state.m4aThreadRunning.load(std::memory_order_acquire);
    });
    initOk = state.m4aInitDone && state.m4aInitOk;
    if (!initOk && error) *error = state.m4aInitError;
  }
  if (!initOk) {
    stopM4aWorker();
    return false;
  }
  return true;
}

}  // namespace

bool initM4aBackend(const std::filesystem::path& file,
                    uint64_t startFrame,
                    int,
                    std::string* error) {
  gAudio.state.totalFrames.store(0, std::memory_order_relaxed);
  return startM4aWorker(file, startFrame, error);
}

void uninitM4aBackend() { stopM4aWorker(); }

bool totalM4aBackend(uint64_t* outFrames) {
  if (!outFrames) return false;
  *outFrames = gAudio.state.totalFrames.load(std::memory_order_relaxed);
  return true;
}
