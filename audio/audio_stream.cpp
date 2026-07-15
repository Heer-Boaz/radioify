#include "audioplayback_internal.h"

extern "C" {
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include "audioplayback.h"
#include "melodyanalysiscache.h"
#include "pipeline_transition.h"
#include "playback_device.h"
#include "playback_source_priming.h"
#include "queued_audio_source.h"
#include "runtime_helpers.h"

bool audioStartStream(uint64_t totalFrames) {
  melodyOfflineStop();
  gAudio.lastInitError.clear();
  gAudio.gmeWarning.clear();
  gAudio.gsfWarning.clear();
  gAudio.vgmWarning.clear();
  gAudio.trackIndex = 0;
  drainPlaybackPipelineForReplacement(&gAudio.state);
  gAudio.state.sourcePreparing.store(true, std::memory_order_release);
  stopAndUninitActiveDecoder();

  const uint32_t rbFrames = std::max<uint32_t>(gAudio.sampleRate, 8192);
  const PlaybackSourcePriming priming =
      playbackSourcePrimingForRate(gAudio.sampleRate);
  const uint32_t targetFrames = std::min<uint32_t>(
      rbFrames, std::max<uint32_t>(gAudio.sampleRate / 2,
                                   priming.targetFrames));
  gAudio.state.channels = gAudio.channels;
  gAudio.state.sampleRate = gAudio.sampleRate;
  queuedAudioSourceStartProcessing(
      &gAudio.state, rbFrames, targetFrames, priming.primeFrames, 0, 0);
  gAudio.state.streamClockReady.store(false);
  gAudio.state.streamStarved.store(false);
  gAudio.state.audioClock.reset(0);
  gAudio.state.sourceAtEnd.store(false);
  gAudio.state.processedAtEnd.store(false);
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
  gAudio.state.outputSafety = {};
  audioPipelineTransitionReset(gAudio.state.pipelineTransition);
  audioPipelineTransitionRequestSignalFadeIn(gAudio.state.pipelineTransition,
                                             gAudio.state.sampleRate);

  if (!audioPlaybackDeviceEnsureRunning()) {
    stopAndUninitActiveDecoder();
    gAudio.state.sourcePreparing.store(false, std::memory_order_release);
    audioPipelineTransitionReset(gAudio.state.pipelineTransition);
    return false;
  }
  audioPlaybackDeviceLatencyFrames();
  gAudio.state.sourcePreparing.store(false, std::memory_order_release);

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
  return static_cast<size_t>(
      gAudio.state.processedAudio.bufferedFrames());
}

size_t audioStreamCapacityFrames() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return static_cast<size_t>(
      gAudio.state.processedAudio.capacityFrames());
}

int64_t audioStreamOldestPtsUs() {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return AV_NOPTS_VALUE;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return AV_NOPTS_VALUE;
  }

  const uint64_t currentRpos =
      gAudio.state.processedAudio.readPosition();
  AudioTimelineAnchor anchor;
  if (!gAudio.state.processedTimeline.findAnchorForFramePosition(
          currentRpos, &anchor)) {
    return AV_NOPTS_VALUE;
  }
  if (currentRpos < anchor.framePosition) return anchor.ptsUs;
  const uint64_t offset = currentRpos - anchor.framePosition;
  return anchor.ptsUs +
         static_cast<int64_t>(offset * 1000000ULL / gAudio.state.sampleRate);
}

bool audioStreamWriteSamples(const float* interleaved, uint64_t frames,
                             int64_t ptsUs, int serial, bool allowBlock,
                             uint64_t* writtenFrames) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return false;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return false;
  }
  return queuedAudioSourceWrite(&gAudio.state, interleaved, frames, ptsUs,
                                serial, allowBlock, false, writtenFrames);
}

void audioStreamPrimeClock(int serial, int64_t targetPtsUs) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load() ||
      !gAudio.state.streamQueueEnabled.load()) {
    return;
  }
  if (serial != gAudio.state.streamSerial.load(std::memory_order_relaxed)) {
    return;
  }

  gAudio.state.audioClock.set(targetPtsUs, nowUs(), serial);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
}

void audioStreamSetEnd(bool atEnd) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  gAudio.state.sourceAtEnd.store(atEnd);
  if (!atEnd) {
    gAudio.state.processedAtEnd.store(false, std::memory_order_relaxed);
  }
  gAudio.state.radioDspCv.notify_all();
}

void audioStreamReset(uint64_t framePos) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  gAudio.state.finished.store(false);
  gAudio.state.sourceAtEnd.store(false);
  gAudio.state.processedAtEnd.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingSeekFrames.store(static_cast<int64_t>(framePos));
  const uint64_t generation = queuedAudioSourceRequestStreamReset(
      &gAudio.state,
      gAudio.state.streamSerial.load(std::memory_order_relaxed), 0, framePos,
      true);
  queuedAudioSourceWaitForStreamReset(&gAudio.state, generation);
}

void audioStreamFlushSerial(int serial, int64_t discardUntilUs) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  queuedAudioSourceRequestStreamReset(
      &gAudio.state, serial, discardUntilUs,
      gAudio.state.framesPlayed.load(std::memory_order_relaxed), false);
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
  std::unique_lock<std::mutex> lock(gAudio.state.streamUpdateMutex);
  gAudio.state.streamUpdateCv.wait_for(
      lock, std::chrono::milliseconds(timeoutMs), [&]() {
        return gAudio.state.streamUpdateCounter.load(
                   std::memory_order_acquire) != lastCounter;
      });
  return gAudio.state.streamUpdateCounter.load(std::memory_order_acquire);
}

uint64_t audioStreamUpdateCounter() {
  return gAudio.state.streamUpdateCounter.load(std::memory_order_acquire);
}
