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
  stopAndUninitActiveDecoder();

  const uint32_t rbFrames = std::max<uint32_t>(gAudio.sampleRate, 8192);
  gAudio.state.streamRb.init(rbFrames, gAudio.channels);
  gAudio.state.streamQueueEnabled.store(true);
  gAudio.state.streamSerial.store(0);
  gAudio.state.pendingStreamSerial.store(0);
  gAudio.state.pendingStreamDiscardPtsUs.store(0);
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

  std::lock_guard<std::mutex> lock(gAudio.state.streamMetadataMutex);
  if (gAudio.state.streamMetadata.empty()) {
    return AV_NOPTS_VALUE;
  }

  const auto& md = gAudio.state.streamMetadata.front();
  uint64_t currentRpos =
      gAudio.state.streamRb.rpos.load(std::memory_order_relaxed);
  if (currentRpos < md.wpos) {
    return md.ptsUs;
  }
  uint64_t offset = currentRpos - md.wpos;
  return md.ptsUs +
         static_cast<int64_t>(offset * 1000000ULL / gAudio.state.sampleRate);
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
  return queuedAudioSourceWrite(&gAudio.state, interleaved, frames, ptsUs,
                                serial, allowBlock, writtenFrames);
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
  gAudio.state.pendingStreamDiscardPtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.streamSerialFlushPending.store(false, std::memory_order_relaxed);
  gAudio.state.pendingSeekFrames.store(static_cast<int64_t>(framePos));
  gAudio.state.audioPrimed.store(false);
  gAudio.state.streamBaseValid.store(false, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.audioClock.reset(
      gAudio.state.streamSerial.load(std::memory_order_relaxed));
  queuedAudioSourceClearMetadata(&gAudio.state);
  gAudio.state.radioResetPending.store(true, std::memory_order_relaxed);
  audioPipelineTransitionRequestSignalFadeIn(gAudio.state.pipelineTransition,
                                             gAudio.state.sampleRate);
  gAudio.state.decodeCv.notify_all();
}

void audioStreamFlushSerial(int serial, int64_t discardUntilUs) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  int64_t targetUs = (std::max)(int64_t{0}, discardUntilUs);
  gAudio.state.pendingStreamSerial.store(serial, std::memory_order_relaxed);
  gAudio.state.pendingStreamDiscardPtsUs.store(targetUs,
                                               std::memory_order_relaxed);
  gAudio.state.streamDiscardPtsUs.store(targetUs, std::memory_order_relaxed);
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
