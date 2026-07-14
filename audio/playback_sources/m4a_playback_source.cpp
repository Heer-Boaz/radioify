#include "m4a_playback_source.h"

#include "../audioplayback_internal.h"
#include "../playback_source_priming.h"
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
  if (!state.m4aThreadRunning.load()) return;
  state.m4aStop.store(true);
  state.m4aCv.notify_all();
  if (state.m4aThread.joinable()) {
    state.m4aThread.join();
  }
  state.m4aThreadRunning.store(false);
  state.m4aStop.store(false);
  state.sourceAtEnd.store(false);
  {
    std::lock_guard<std::mutex> lock(state.m4aMutex);
    state.m4aBuffer.reset();
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
  const PlaybackSourcePriming priming =
      playbackSourcePrimingForRate(gAudio.sampleRate);

  {
    std::lock_guard<std::mutex> lock(state.m4aMutex);
    state.m4aBuffer.init(priming.capacityFrames, gAudio.channels);
    state.m4aInitDone = false;
    state.m4aInitOk = false;
    state.m4aInitError.clear();
  }
  state.m4aStop.store(false);
  state.sourceAtEnd.store(false);
  state.m4aThreadRunning.store(true);

  const uint32_t workerChannels = gAudio.channels;
  const uint32_t workerRate = gAudio.sampleRate;
  state.m4aThread = std::thread([file, startFrame, workerChannels, workerRate,
                                 targetFrames = priming.targetFrames,
                                 primeFrames = priming.primeFrames]() {
    AudioState& state = gAudio.state;
    FfmpegAudioDecoder decoder;
    std::string initError;
#if RADIOIFY_ENABLE_TIMING_LOG
    auto t_init_start = std::chrono::steady_clock::now();
#endif
    bool ok = decoder.init(file, workerChannels, workerRate, &initError);
#if RADIOIFY_ENABLE_TIMING_LOG
    auto t_init_end = std::chrono::steady_clock::now();
    {
      char buf[512];
      std::snprintf(buf, sizeof(buf),
                    "ffmpeg_audio_init_ms=%lld file=%s ok=%d err=%s",
                    static_cast<long long>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            t_init_end - t_init_start)
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
        startOffsetFrames = 0;
      } else if (startOffsetFrames < 0) {
        uint64_t extraSkip = static_cast<uint64_t>(-startOffsetFrames);
        paddingFrames += extraSkip;
        if (total > extraSkip) {
          total -= extraSkip;
        } else {
          total = 0;
        }
        startOffsetFrames = 0;
      }
      if (total > 0 && localStart > total) {
        localStart = total;
      }
      if (total > 0) {
        rawTotalFrames = total + paddingFrames + trailingFrames;
      }
      uint64_t rawStart = localStart + paddingFrames;
      if (rawTotalFrames > 0 && rawStart > rawTotalFrames) {
        rawStart = rawTotalFrames;
      }
      if (rawStart > 0) {
        if (!decoder.seekToFrame(rawStart)) {
          localStart = 0;
          decoder.seekToFrame(0);
        }
      }
    }

    {
      std::lock_guard<std::mutex> lock(state.m4aMutex);
      state.m4aInitDone = true;
      state.m4aInitOk = ok;
      state.m4aInitError = initError;
    }
    state.totalFrames.store(total);
    state.audioPaddingFrames.store(paddingFrames);
    state.audioTrailingPaddingFrames.store(trailingFrames);
    state.audioLeadSilenceFrames.store(leadSilenceFrames);
    state.framesPlayed.store(localStart);
    state.audioClockFrames.store(localStart);
    state.m4aCv.notify_all();
    if (!ok) {
      return;
    }

    constexpr uint32_t kChunkFrames = 2048;
    std::vector<float> buffer(static_cast<size_t>(kChunkFrames) *
                              workerChannels);
    bool m4a_first_buffer_logged = false;
    bool seekCommitPending = false;

    auto finishSeekCommitWhenPrimed = [&]() {
      if (!seekCommitPending) return;
      size_t bufferedFrames = 0;
      {
        std::lock_guard<std::mutex> lock(state.m4aMutex);
        bufferedFrames = state.m4aBuffer.size();
      }
      const bool atEnd = state.sourceAtEnd.load(std::memory_order_relaxed);
      if (!playbackSourceIsPrimed(bufferedFrames, primeFrames, atEnd)) {
        return;
      }
      audioPlaybackFinishSeekPipelineTransition(&state, false);
      seekCommitPending = false;
      state.m4aCv.notify_all();
    };

    while (!state.m4aStop.load()) {
      if (!seekCommitPending &&
          state.seekRequested.load(std::memory_order_relaxed) &&
          audioPipelineTransitionBeginCommit(state.pipelineTransition)) {
        int64_t target = state.pendingSeekFrames.load();
        if (target < 0) target = 0;
        uint64_t seekTargetFrames = static_cast<uint64_t>(target);
        uint64_t rawTarget = seekTargetFrames + paddingFrames;
        if (rawTotalFrames > 0 && rawTarget > rawTotalFrames) {
          rawTarget = rawTotalFrames;
        }
        if (!decoder.seekToFrame(rawTarget)) {
          rawTarget = 0;
          seekTargetFrames = 0;
          decoder.seekToFrame(0);
        }
        state.framesPlayed.store(seekTargetFrames);
        state.audioClockFrames.store(seekTargetFrames);
        state.finished.store(false);
        state.sourceAtEnd.store(false);
        state.audioLeadSilenceFrames.store(0);
        {
          std::lock_guard<std::mutex> lock(state.m4aMutex);
          state.m4aBuffer.reset();
        }
        state.radioResetPending.store(true, std::memory_order_relaxed);
        audioPipelineTransitionRequestSignalFadeIn(state.pipelineTransition,
                                                   state.sampleRate);
        seekCommitPending = true;
        state.m4aCv.notify_all();
        continue;
      }

      if (!seekCommitPending &&
          audioPipelineTransitionActive(state.pipelineTransition)) {
        std::unique_lock<std::mutex> lock(state.m4aMutex);
        state.m4aCv.wait_for(lock, std::chrono::milliseconds(5), [&]() {
          return state.m4aStop.load() ||
                 !audioPipelineTransitionActive(state.pipelineTransition);
        });
        continue;
      }

      if (state.sourceAtEnd.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        continue;
      }

      size_t spaceFrames = 0;
      size_t bufferedFrames = 0;
      {
        std::lock_guard<std::mutex> lock(state.m4aMutex);
        spaceFrames = state.m4aBuffer.space();
        bufferedFrames = state.m4aBuffer.size();
      }
      if (spaceFrames == 0 || bufferedFrames >= targetFrames) {
        std::unique_lock<std::mutex> lock(state.m4aMutex);
        state.m4aCv.wait_for(lock, std::chrono::milliseconds(5), [&]() {
          return (state.m4aBuffer.space() > 0 &&
                  state.m4aBuffer.size() < targetFrames) ||
                 state.m4aStop.load() ||
                 audioPipelineTransitionActive(state.pipelineTransition);
        });
        continue;
      }

      size_t desiredFrames =
          targetFrames > bufferedFrames ? targetFrames - bufferedFrames : 0;
      size_t toRead = std::min<size_t>(spaceFrames, kChunkFrames);
      if (desiredFrames > 0) {
        toRead = std::min(toRead, desiredFrames);
      }
      uint32_t framesToRead = static_cast<uint32_t>(toRead);
      uint64_t framesRead = 0;
      if (!decoder.readFrames(buffer.data(), framesToRead, &framesRead)) {
        state.sourceAtEnd.store(true);
        state.m4aCv.notify_all();
        finishSeekCommitWhenPrimed();
        continue;
      }
      if (framesRead == 0) {
        state.sourceAtEnd.store(true);
        state.m4aCv.notify_all();
        finishSeekCommitWhenPrimed();
        continue;
      }

      if (!m4a_first_buffer_logged) {
        m4a_first_buffer_logged = true;
#if RADIOIFY_ENABLE_TIMING_LOG
        auto t_buf = std::chrono::steady_clock::now();
        {
          char buf[512];
          std::snprintf(
              buf, sizeof(buf),
              "ffmpeg_audio_first_buffer_ms=%lld frames=%llu file=%s",
              static_cast<long long>(
                  std::chrono::duration_cast<std::chrono::milliseconds>(
                      t_buf - t_init_start)
                      .count()),
              static_cast<unsigned long long>(framesRead),
              toUtf8String(file.filename()).c_str());
          appendAudioTimingLogLine(buf);
        }
#endif
      }

      if (!state.dry) {
        audioPlaybackProcessRadioBlock(&state, buffer.data(),
                                       static_cast<uint32_t>(framesRead));
      }
      {
        std::lock_guard<std::mutex> lock(state.m4aMutex);
        state.m4aBuffer.write(buffer.data(), static_cast<size_t>(framesRead));
      }
      state.m4aCv.notify_all();
      finishSeekCommitWhenPrimed();
    }
  });

  bool initOk = false;
  {
    std::unique_lock<std::mutex> lock(state.m4aMutex);
    state.m4aCv.wait(lock, [&]() {
      if (!state.m4aInitDone) return false;
      if (!state.m4aInitOk) return true;
      const bool atEnd = state.sourceAtEnd.load(std::memory_order_relaxed);
      return playbackSourceIsPrimed(state.m4aBuffer.size(), priming.primeFrames,
                                    atEnd);
    });
    initOk = state.m4aInitOk;
    if (!initOk && error) {
      *error = state.m4aInitError;
    }
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
  gAudio.state.totalFrames.store(0);
  return startM4aWorker(file, startFrame, error);
}

void uninitM4aBackend() {
  stopM4aWorker();
}

bool readM4aBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!out || frameCount == 0) return false;

  size_t readFrames = 0;
  {
    std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
    readFrames = gAudio.state.m4aBuffer.read(out, frameCount);
  }
  if (framesRead) *framesRead = static_cast<uint64_t>(readFrames);
  gAudio.state.m4aCv.notify_one();
  return true;
}

bool totalM4aBackend(uint64_t* outFrames) {
  if (!outFrames) return false;
  uint64_t total = gAudio.state.totalFrames.load();
  *outFrames = total;
  return true;
}
