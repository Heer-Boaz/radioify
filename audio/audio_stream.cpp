#include "audio_stream.h"

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

bool audioStartStream(AudioPlaybackState& audio, uint64_t totalFrames) {
  melodyOfflineStop();
  audio.lastInitError.clear();
  audio.gmeWarning.clear();
  audio.gsfWarning.clear();
  audio.vgmWarning.clear();
  audio.trackIndex = 0;
  drainPlaybackPipelineForReplacement(audio);
  audio.state.sourcePreparing.store(true, std::memory_order_release);
  stopAndUninitActiveDecoder(audio);

  const uint32_t rbFrames = std::max<uint32_t>(audio.sampleRate, 8192);
  const PlaybackSourcePriming priming =
      playbackSourcePrimingForRate(audio.sampleRate);
  const uint32_t targetFrames = std::min<uint32_t>(
      rbFrames, std::max<uint32_t>(audio.sampleRate / 2,
                                   priming.targetFrames));
  audio.state.channels = audio.channels;
  audio.state.sampleRate = audio.sampleRate;
  queuedAudioSourceStartProcessing(
      &audio.state, rbFrames, targetFrames, priming.primeFrames, 0, 0);
  audio.state.streamClockReady.store(false);
  audio.state.streamStarved.store(false);
  audio.state.audioClock.reset(0);
  audio.state.sourceAtEnd.store(false);
  audio.state.processedAtEnd.store(false);
  audio.state.externalStream.store(true);
  audio.state.mode.store(AudioMode::Stream, std::memory_order_relaxed);
  audio.state.backend = nullptr;
  audio.decoderReady = true;
  audio.trackIndex = 0;

  audio.state.framesPlayed.store(0);
  audio.state.audioClockFrames.store(0);
  audio.state.callbackCount.store(0);
  audio.state.framesRequested.store(0);
  audio.state.framesReadTotal.store(0);
  audio.state.shortReadCount.store(0);
  audio.state.silentFrames.store(0);
  audio.state.pausedCallbacks.store(0);
  audio.state.lastCallbackFrames.store(0);
  audio.state.lastFramesRead.store(0);
  audio.state.audioPrimed.store(false);
  audio.state.seekRequested.store(false);
  audio.state.pendingSeekFrames.store(0);
  audio.state.seekPresentation.reset();
  audio.state.finished.store(false);
  audio.state.paused.store(false);
  audio.state.hold.store(false);
  audio.state.audioPaddingFrames.store(0);
  audio.state.audioTrailingPaddingFrames.store(0);
  audio.state.audioLeadSilenceFrames.store(0);
  audio.state.totalFrames.store(totalFrames);
  audioPipelineTransitionReset(audio.state.pipelineTransition);
  audioPipelineTransitionRequestSignalFadeIn(audio.state.pipelineTransition,
                                             audio.state.sampleRate);

  if (!audioPlaybackDeviceEnsureRunning(audio)) {
    stopAndUninitActiveDecoder(audio);
    audio.state.sourcePreparing.store(false, std::memory_order_release);
    audioPipelineTransitionReset(audio.state.pipelineTransition);
    return false;
  }
  audioPlaybackDeviceLatencyFrames(audio);
  audio.state.sourcePreparing.store(false, std::memory_order_release);

  audio.nowPlaying.clear();
  return true;
}

void audioStopStream(AudioPlaybackState& audio) {
  audio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  stopPlayback(audio);
}

size_t audioStreamBufferedFrames(AudioPlaybackState& audio) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return 0;
  }
  if (!audio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return static_cast<size_t>(
      audio.state.processedAudio.bufferedFrames());
}

size_t audioStreamCapacityFrames(AudioPlaybackState& audio) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return 0;
  }
  if (!audio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return static_cast<size_t>(
      audio.state.processedAudio.capacityFrames());
}

int64_t audioStreamOldestPtsUs(AudioPlaybackState& audio) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return AV_NOPTS_VALUE;
  }
  if (!audio.state.streamQueueEnabled.load()) {
    return AV_NOPTS_VALUE;
  }

  const uint64_t currentRpos =
      audio.state.processedAudio.readPosition();
  AudioTimelineAnchor anchor;
  if (!audio.state.processedTimeline.findAnchorForFramePosition(
          currentRpos, &anchor)) {
    return AV_NOPTS_VALUE;
  }
  if (currentRpos < anchor.framePosition) return anchor.ptsUs;
  const uint64_t offset = currentRpos - anchor.framePosition;
  return anchor.ptsUs +
         static_cast<int64_t>(offset * 1000000ULL / audio.state.sampleRate);
}

bool audioStreamWriteSamples(AudioPlaybackState& audio,
                             const float* interleaved, uint64_t frames,
                             int64_t ptsUs, int serial, bool allowBlock,
                             uint64_t* writtenFrames) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return false;
  }
  if (!audio.state.streamQueueEnabled.load()) {
    return false;
  }
  return queuedAudioSourceWrite(&audio.state, interleaved, frames, ptsUs,
                                serial, allowBlock, false, writtenFrames);
}

void audioStreamPrimeClock(AudioPlaybackState& audio, int serial,
                           int64_t targetPtsUs) {
  if (!audio.decoderReady || !audio.state.externalStream.load() ||
      !audio.state.streamQueueEnabled.load()) {
    return;
  }
  if (serial != audio.state.streamSerial.load(std::memory_order_relaxed)) {
    return;
  }

  audio.state.audioClock.set(targetPtsUs, nowUs(), serial);
  audio.state.streamClockReady.store(false, std::memory_order_relaxed);
  audio.state.streamStarved.store(false, std::memory_order_relaxed);
}

void audioStreamSetEnd(AudioPlaybackState& audio, bool atEnd) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return;
  }
  audio.state.sourceAtEnd.store(atEnd);
  if (!atEnd) {
    audio.state.processedAtEnd.store(false, std::memory_order_relaxed);
  }
  audio.state.radioDspCv.notify_all();
}

void audioStreamReset(AudioPlaybackState& audio, uint64_t framePos) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return;
  }
  audio.state.finished.store(false);
  audio.state.sourceAtEnd.store(false);
  audio.state.processedAtEnd.store(false);
  audio.state.seekRequested.store(false);
  audio.state.pendingSeekFrames.store(static_cast<int64_t>(framePos));
  const uint64_t generation = queuedAudioSourceRequestStreamReset(
      &audio.state,
      audio.state.streamSerial.load(std::memory_order_relaxed), 0, framePos,
      true);
  queuedAudioSourceWaitForStreamReset(&audio.state, generation);
}

void audioStreamFlushSerial(AudioPlaybackState& audio, int serial,
                            int64_t discardUntilUs) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return;
  }
  queuedAudioSourceRequestStreamReset(
      &audio.state, serial, discardUntilUs,
      audio.state.framesPlayed.load(std::memory_order_relaxed), false);
}

AudioStreamReset audioStreamLastAppliedReset(AudioPlaybackState& audio) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return {};
  }
  if (!audio.state.streamQueueEnabled.load()) {
    return {};
  }
  return queuedAudioSourceLastAppliedStreamReset(&audio.state);
}

int audioStreamSerial(AudioPlaybackState& audio) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return 0;
  }
  if (!audio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return audio.state.streamSerial.load(std::memory_order_relaxed);
}

int64_t audioStreamClockUs(AudioPlaybackState& audio, int64_t nowUs) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return 0;
  }
  if (!audio.state.streamQueueEnabled.load()) {
    return 0;
  }
  if (!audio.state.audioClock.is_valid()) {
    return 0;
  }

  return audio.state.audioClock.get(nowUs);
}

int64_t audioStreamClockLastUpdatedUs(AudioPlaybackState& audio) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return 0;
  }
  if (!audio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return audio.state.audioClock.last_updated_us.load(std::memory_order_relaxed);
}

bool audioStreamStarved(AudioPlaybackState& audio) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return false;
  }
  if (!audio.state.streamQueueEnabled.load()) {
    return false;
  }
  return audio.state.streamStarved.load(std::memory_order_relaxed);
}

bool audioStreamClockReady(AudioPlaybackState& audio) {
  if (!audio.decoderReady || !audio.state.externalStream.load()) {
    return false;
  }
  if (!audio.state.streamQueueEnabled.load()) {
    return false;
  }
  return audio.state.streamClockReady.load(std::memory_order_relaxed);
}

uint64_t audioStreamWaitForUpdate(AudioPlaybackState& audio,
                                  uint64_t lastCounter, int timeoutMs) {
  std::unique_lock<std::mutex> lock(audio.state.streamUpdateMutex);
  audio.state.streamUpdateCv.wait_for(
      lock, std::chrono::milliseconds(timeoutMs), [&]() {
        return audio.state.streamUpdateCounter.load(
                   std::memory_order_acquire) != lastCounter;
      });
  return audio.state.streamUpdateCounter.load(std::memory_order_acquire);
}

uint64_t audioStreamUpdateCounter(AudioPlaybackState& audio) {
  return audio.state.streamUpdateCounter.load(std::memory_order_acquire);
}
