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
#include "playback_device.h"
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

AudioPlaybackState gAudio;

void appendAudioTimingLogLine(const char* line) {
#if RADIOIFY_ENABLE_TIMING_LOG
  if (!line || line[0] == '\0') return;
  std::ofstream f(radioifyLogPath().string(), std::ios::app);
  if (f) f << radioifyLogTimestamp() << " " << line << "\n";
#else
  (void)line;
#endif
}

void audioRingBufferInit(AudioRingBuffer* buffer, size_t frames,
                         uint32_t channels) {
  if (!buffer) return;
  buffer->capacityFrames = frames;
  buffer->data.assign(frames * channels, 0.0f);
  buffer->readPos = 0;
  buffer->writePos = 0;
  buffer->bufferedFrames = 0;
}

void audioRingBufferReset(AudioRingBuffer* buffer) {
  if (!buffer) return;
  buffer->readPos = 0;
  buffer->writePos = 0;
  buffer->bufferedFrames = 0;
}

size_t audioRingBufferSpace(const AudioRingBuffer* buffer) {
  if (!buffer) return 0;
  if (buffer->capacityFrames < buffer->bufferedFrames) return 0;
  return buffer->capacityFrames - buffer->bufferedFrames;
}

size_t audioRingBufferSize(const AudioRingBuffer* buffer) {
  if (!buffer) return 0;
  return buffer->bufferedFrames;
}

size_t audioRingBufferRead(AudioRingBuffer* buffer, float* out, size_t frames,
                           uint32_t channels) {
  if (!buffer || !out || buffer->capacityFrames == 0 || frames == 0) return 0;
  size_t toRead = std::min(frames, buffer->bufferedFrames);
  if (toRead == 0) return 0;
  size_t first = std::min(toRead, buffer->capacityFrames - buffer->readPos);
  size_t firstSamples = first * channels;
  std::memcpy(out, buffer->data.data() + buffer->readPos * channels,
              firstSamples * sizeof(float));
  if (toRead > first) {
    size_t second = toRead - first;
    std::memcpy(out + firstSamples, buffer->data.data(),
                second * channels * sizeof(float));
  }
  buffer->readPos = (buffer->readPos + toRead) % buffer->capacityFrames;
  buffer->bufferedFrames -= toRead;
  return toRead;
}

size_t audioRingBufferWrite(AudioRingBuffer* buffer, const float* in,
                            size_t frames, uint32_t channels) {
  if (!buffer || !in || buffer->capacityFrames == 0 || frames == 0) return 0;
  size_t spaceFrames = audioRingBufferSpace(buffer);
  size_t toWrite = std::min(frames, spaceFrames);
  if (toWrite == 0) return 0;
  size_t first = std::min(toWrite, buffer->capacityFrames - buffer->writePos);
  size_t firstSamples = first * channels;
  std::memcpy(buffer->data.data() + buffer->writePos * channels, in,
              firstSamples * sizeof(float));
  if (toWrite > first) {
    size_t second = toWrite - first;
    std::memcpy(buffer->data.data(), in + firstSamples,
                second * channels * sizeof(float));
  }
  buffer->writePos = (buffer->writePos + toWrite) % buffer->capacityFrames;
  buffer->bufferedFrames += toWrite;
  return toWrite;
}

static constexpr uint32_t kRadioProcessChannels = 1u;
static void audioStreamProcessRadioImpl(float* interleaved, uint32_t frames);
static bool backendUsesQueuedDecodeThread(const AudioBackendHandlers* backend);
static void clearQueuedPlaybackMetadata(AudioState* state);
static void resetQueuedPlaybackState(AudioState* state,
                                     uint64_t framePos,
                                     int serial);
static void stopDecodeWorker();
static bool writeQueuedSamples(AudioState* state,
                               const float* interleaved,
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
      audioStreamProcessRadioImpl(buffer.data(), static_cast<uint32_t>(kChunkFrames));
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
  const uint32_t channels = std::max(1u, state->channels);
  state->radioPreview.runBlock(state->radio1938, samples, frames, channels);
  if (state->radio1938.diagnostics.anyClip) {
    holdClipAlert(state);
  }
}

static void audioStreamProcessRadioImpl(float* interleaved, uint32_t frames) {
  if (!interleaved || frames == 0) return;
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) return;
  if (!gAudio.state.streamQueueEnabled.load()) return;
  if (gAudio.state.dry || !gAudio.state.useRadio1938.load()) return;
  processRadioBlock(&gAudio.state, interleaved, frames);
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
  state->m4aAtEnd.store(false, std::memory_order_relaxed);
  state->audioPrimed.store(false, std::memory_order_relaxed);
  state->streamBaseValid.store(false, std::memory_order_relaxed);
  state->streamBasePtsUs.store(0, std::memory_order_relaxed);
  state->streamReadFrames.store(0, std::memory_order_relaxed);
  state->streamClockReady.store(false, std::memory_order_relaxed);
  state->streamStarved.store(false, std::memory_order_relaxed);
  state->streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  state->audioClock.reset(serial);
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
           state->seekRequested.load(std::memory_order_relaxed) ||
           !state->streamQueueEnabled.load(std::memory_order_relaxed);
  });
  return !state->decodeStop.load(std::memory_order_relaxed) &&
         !state->seekRequested.load(std::memory_order_relaxed) &&
         state->streamQueueEnabled.load(std::memory_order_relaxed);
}

static bool writeQueuedSamples(AudioState* state,
                               const float* interleaved,
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
  const float* cursor = interleaved;
  uint64_t totalWritten = 0;
  while (remaining > 0) {
    if (!state->externalStream.load(std::memory_order_relaxed) &&
        state->seekRequested.load(std::memory_order_relaxed)) {
      return false;
    }
    if (serial != state->streamSerial.load(std::memory_order_relaxed)) {
      return true;
    }
    if (!state->streamQueueEnabled.load(std::memory_order_relaxed)) {
      return false;
    }
    uint64_t written = state->streamRb.writeSome(cursor, remaining);
    if (written == 0) {
      if (!allowBlock) {
        break;
      }
      std::unique_lock<std::mutex> lock(state->decodeMutex);
      state->decodeCv.wait_for(lock, std::chrono::milliseconds(5), [&]() {
        return state->decodeStop.load(std::memory_order_relaxed) ||
               state->seekRequested.load(std::memory_order_relaxed) ||
               !state->streamQueueEnabled.load(std::memory_order_relaxed) ||
               serial != state->streamSerial.load(std::memory_order_relaxed) ||
               state->streamRb.writableFrames() > 0;
      });
      continue;
    }
    remaining -= written;
    totalWritten += written;
    cursor += static_cast<size_t>(written) * state->channels;
  }
  if (writtenFrames) {
    *writtenFrames = totalWritten;
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
  const bool finishOnShortRead = backend->finishOnShortRead;
  gAudio.state.decodeThread =
      std::thread([backend, startFrame, workerChannels, workerRate,
                   finishOnShortRead]() {
        constexpr uint32_t kChunkFrames = 2048;
        constexpr uint32_t kQueuedDecodeEmptyReadLimit = 32;
        std::vector<float> buffer(static_cast<size_t>(kChunkFrames) *
                                  workerChannels);
        uint64_t framePos = startFrame;
        uint32_t consecutiveEmptyReads = 0;
        while (!gAudio.state.decodeStop.load(std::memory_order_relaxed)) {
          if (backend->seek &&
              gAudio.state.seekRequested.exchange(false,
                                                 std::memory_order_relaxed)) {
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
            continue;
          }

          uint64_t writable = gAudio.state.streamRb.writableFrames();
          if (writable == 0) {
            std::unique_lock<std::mutex> lock(gAudio.state.decodeMutex);
            gAudio.state.decodeCv.wait_for(
                lock, std::chrono::milliseconds(5), [&]() {
                  return gAudio.state.decodeStop.load(std::memory_order_relaxed) ||
                         gAudio.state.seekRequested.load(std::memory_order_relaxed) ||
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
            gAudio.state.m4aAtEnd.store(true, std::memory_order_relaxed);
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
            if (!gAudio.state.dry && gAudio.state.useRadio1938.load()) {
              processRadioBlock(&gAudio.state, buffer.data(),
                                static_cast<uint32_t>(framesRead));
            }
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
            gAudio.state.m4aAtEnd.store(true, std::memory_order_relaxed);
            break;
          }
        }
        gAudio.state.decodeThreadRunning.store(false, std::memory_order_relaxed);
        gAudio.state.decodeCv.notify_all();
      });
  return true;
}

static inline float softLimitOutputSample(float x, float threshold = 0.985f) {
  float ax = std::fabs(x);
  if (ax <= threshold) return x;
  float sign = (x < 0.0f) ? -1.0f : 1.0f;
  float denom = std::max(1e-6f, 1.0f - threshold);
  float u = (ax - threshold) / denom;
  float y = threshold + (1.0f - std::exp(-u)) * (1.0f - threshold);
  return sign * y;
}

static bool applyOutputVolumeSafety(float* samples,
                                    uint32_t frames,
                                    uint32_t channels,
                                    float volume) {
  if (!samples || frames == 0 || channels == 0) return false;
  constexpr float kOutputClamp = 0.999f;
  bool limited = false;
  size_t count = static_cast<size_t>(frames) * channels;
  for (size_t i = 0; i < count; ++i) {
    float x = samples[i] * volume;
    if (std::fabs(x) > 1.0f) limited = true;
    x = softLimitOutputSample(x);
    if (std::fabs(x) >= kOutputClamp) limited = true;
    samples[i] = std::clamp(x, -kOutputClamp, kOutputClamp);
  }
  return limited;
}

void dataCallback(ma_device* device, void* output, const void*,
                  ma_uint32 frameCount) {
  auto* state = static_cast<AudioState*>(device->pUserData);
  float* out = static_cast<float*>(output);
  if (!state) return;

  state->callbackCount.fetch_add(1, std::memory_order_relaxed);
  state->framesRequested.fetch_add(frameCount, std::memory_order_relaxed);
  state->lastCallbackFrames.store(frameCount, std::memory_order_relaxed);
  state->audioPrimed.store(true, std::memory_order_relaxed);

  bool useStreamQueue = state->streamQueueEnabled.load();
  if (useStreamQueue) {
    const uint32_t channels = state->channels;
    const bool paused = state->paused.load() || state->hold.load();
    if (paused) {
      state->pausedCallbacks.fetch_add(1, std::memory_order_relaxed);
      state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
      state->lastFramesRead.store(0, std::memory_order_relaxed);
      std::fill(out, out + frameCount * channels, 0.0f);
      updatePeakMeter(state, out, frameCount);
      state->audioClock.set_paused(true, nowUs());
      state->streamStarved.store(false, std::memory_order_relaxed);
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
        // Task 1: Find the most recent metadata entry that is AT or BEFORE our current read position.
        // This ensures the clock is always tied to the exact chunk of audio being played.
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
        
        state->audioClock.sync_to_pts(currentPts - delayUs, nowUs(), currentSerial);
        state->streamClockReady.store(true, std::memory_order_relaxed);
        
        // Task 3: Signal that audio hardware has progressed (Hardware Callback Driving)
        state->streamUpdateCounter.fetch_add(1, std::memory_order_release);
        state->streamUpdateCv.notify_all();
      }
    }

    bool starved = (framesRead < frameCount);
    state->streamStarved.store(starved, std::memory_order_relaxed);
    
    // If we starved, check how long it's been since the last valid update.
    // High-latency outputs such as Remote Desktop can legitimately go much
    // longer between stable hardware updates than local speakers, so scale the
    // free-run window with the device buffer budget instead of using a hard
    // local-only threshold.
    if (starved) {
      int64_t now = nowUs();
      int64_t last = state->audioClock.last_updated_us.load(std::memory_order_relaxed);
      int64_t graceUs = queuedAudioClockStarvationGraceUs(state, frameCount);
      if (now - last > graceUs) {
        state->audioClock.invalidate();
        state->streamClockReady.store(false, std::memory_order_relaxed);
      }
    }

    if (starved && state->m4aAtEnd.load()) {
      state->finished.store(true);
      uint64_t total = state->totalFrames.load();
      if (total > 0) {
        state->framesPlayed.store(total, std::memory_order_relaxed);
      }
    }

    float vol = state->volume.load(std::memory_order_relaxed);
    if (applyOutputVolumeSafety(out, frameCount, channels, vol)) {
      holdClipAlert(state);
    }
    updatePeakMeter(state, out, frameCount);
    return;
  }

  if (state->hold.load()) {
    state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    std::fill(out, out + frameCount * state->channels, 0.0f);
    updatePeakMeter(state, out, frameCount);
    return;
  }

  const AudioMode mode = state->mode.load(std::memory_order_relaxed);
  const AudioBackendHandlers* backend = state->backend;
  if (backend && backend->allowSeekInCallback && backend->seek &&
      state->seekRequested.exchange(false)) {
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
  }

  if (state->paused.load()) {
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
        if (mode == AudioMode::M4a) {
          if (framesRead == 0 && state->m4aAtEnd.load()) {
            state->finished.store(true, std::memory_order_relaxed);
            uint64_t total = state->totalFrames.load(std::memory_order_relaxed);
            if (total > 0) {
              state->framesPlayed.store(total, std::memory_order_relaxed);
            }
          }
        } else if (backend->finishOnShortRead &&
                   (framesRead == 0 || framesRead < framesRemaining)) {
          finishPlayback();
        }
      }
    }
  }

  state->audioClockFrames.fetch_add(frameCount, std::memory_order_relaxed);
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

  float vol = state->volume.load(std::memory_order_relaxed);
  if (applyOutputVolumeSafety(out, frameCount, state->channels, vol)) {
    holdClipAlert(state);
  }
  updatePeakMeter(state, out, frameCount);
}

void stopM4aWorker() {
  if (!gAudio.state.m4aThreadRunning.load()) return;
  gAudio.state.m4aStop.store(true);
  gAudio.state.m4aCv.notify_all();
  if (gAudio.state.m4aThread.joinable()) {
    gAudio.state.m4aThread.join();
  }
  gAudio.state.m4aThreadRunning.store(false);
  gAudio.state.m4aStop.store(false);
  gAudio.state.m4aAtEnd.store(false);
  {
    std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
    audioRingBufferReset(&gAudio.state.m4aBuffer);
    gAudio.state.m4aInitDone = false;
    gAudio.state.m4aInitOk = false;
    gAudio.state.m4aInitError.clear();
  }
}

bool startM4aWorker(const std::filesystem::path& file, uint64_t startFrame,
                    std::string* error) {
  stopM4aWorker();
  const uint32_t rbFrames = std::max<uint32_t>(gAudio.sampleRate / 2, 8192);
    const uint32_t targetFrames =
      std::min<uint32_t>(rbFrames, std::max<uint32_t>(gAudio.sampleRate / 4, 4096));
  {
    std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
    audioRingBufferInit(&gAudio.state.m4aBuffer, rbFrames, gAudio.channels);
    gAudio.state.m4aInitDone = false;
    gAudio.state.m4aInitOk = false;
    gAudio.state.m4aInitError.clear();
  }
  gAudio.state.m4aStop.store(false);
  gAudio.state.m4aAtEnd.store(false);
  gAudio.state.m4aThreadRunning.store(true);
  const uint32_t workerChannels = gAudio.channels;
  const uint32_t workerRate = gAudio.sampleRate;
  gAudio.state.m4aThread = std::thread([file, startFrame, workerChannels,
                                        workerRate, targetFrames]() {
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
          rawStart = 0;
          localStart = 0;
        }
      }
    }
    {
      std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
      gAudio.state.m4aInitDone = true;
      gAudio.state.m4aInitOk = ok;
      gAudio.state.m4aInitError = initError;
    }
    gAudio.state.totalFrames.store(total);
    gAudio.state.audioPaddingFrames.store(paddingFrames);
    gAudio.state.audioTrailingPaddingFrames.store(trailingFrames);
    gAudio.state.audioLeadSilenceFrames.store(leadSilenceFrames);
    if (localStart > 0) {
      gAudio.state.framesPlayed.store(localStart);
      gAudio.state.audioClockFrames.store(localStart);
    }
    gAudio.state.m4aCv.notify_all();
    if (!ok) {
      return;
    }

    constexpr uint32_t kChunkFrames = 2048;
    std::vector<float> buffer(static_cast<size_t>(kChunkFrames) *
                              workerChannels);
    bool m4a_first_buffer_logged = false;

    while (!gAudio.state.m4aStop.load()) {
      if (gAudio.state.seekRequested.load()) {
        int64_t target = gAudio.state.pendingSeekFrames.load();
        if (target < 0) target = 0;
        gAudio.state.seekRequested.store(false);
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
        gAudio.state.framesPlayed.store(seekTargetFrames);
        gAudio.state.audioClockFrames.store(seekTargetFrames);
        gAudio.state.finished.store(false);
        gAudio.state.m4aAtEnd.store(false);
        gAudio.state.audioLeadSilenceFrames.store(0);
        {
          std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
          audioRingBufferReset(&gAudio.state.m4aBuffer);
        }
      }

      if (gAudio.state.m4aAtEnd.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
        continue;
      }

      size_t spaceFrames = 0;
      size_t bufferedFrames = 0;
      {
        std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
        spaceFrames = audioRingBufferSpace(&gAudio.state.m4aBuffer);
        bufferedFrames = audioRingBufferSize(&gAudio.state.m4aBuffer);
      }
      if (spaceFrames == 0 || bufferedFrames >= targetFrames) {
        std::unique_lock<std::mutex> lock(gAudio.state.m4aMutex);
        gAudio.state.m4aCv.wait_for(lock, std::chrono::milliseconds(5), [&]() {
          return (audioRingBufferSpace(&gAudio.state.m4aBuffer) > 0 &&
                  audioRingBufferSize(&gAudio.state.m4aBuffer) < targetFrames) ||
                 gAudio.state.m4aStop.load() ||
                 gAudio.state.seekRequested.load();
        });
        continue;
      }

      size_t desiredFrames =
          targetFrames > bufferedFrames ? (targetFrames - bufferedFrames) : 0;
      size_t toRead = std::min<size_t>(spaceFrames, kChunkFrames);
      if (desiredFrames > 0) {
        toRead = std::min(toRead, desiredFrames);
      }
      uint32_t framesToRead = static_cast<uint32_t>(toRead);
      uint64_t framesRead = 0;
      if (!decoder.readFrames(buffer.data(), framesToRead, &framesRead)) {
        gAudio.state.m4aAtEnd.store(true);
        continue;
      }
      if (framesRead == 0) {
        gAudio.state.m4aAtEnd.store(true);
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
      {
        std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
        audioRingBufferWrite(&gAudio.state.m4aBuffer, buffer.data(),
                             static_cast<size_t>(framesRead), workerChannels);
      }
    }
  });

  {
    std::unique_lock<std::mutex> lock(gAudio.state.m4aMutex);
    gAudio.state.m4aCv.wait(lock, [&]() { return gAudio.state.m4aInitDone; });
    if (!gAudio.state.m4aInitOk && error) {
      *error = gAudio.state.m4aInitError;
    }
  }
  if (!gAudio.state.m4aInitOk) {
    stopM4aWorker();
    return false;
  }
  return true;
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

bool initM4aBackend(const std::filesystem::path& file, uint64_t startFrame,
                    int, std::string* error) {
  gAudio.state.totalFrames.store(0);
  return startM4aWorker(file, startFrame, error);
}

bool initFfmpegBackend(const std::filesystem::path& file, uint64_t, int,
                       std::string* error) {
  gAudio.state.totalFrames.store(0);
  return gAudio.state.ffmpeg.init(file, gAudio.channels, gAudio.sampleRate, error);
}

void uninitM4aBackend() { stopM4aWorker(); }

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

bool readM4aBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!out || frameCount == 0) return false;

  if (!gAudio.state.externalStream.load() && gAudio.state.seekRequested.load()) {
    std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
    audioRingBufferReset(&gAudio.state.m4aBuffer);
    gAudio.state.m4aAtEnd.store(false);
    gAudio.state.finished.store(false);
  }

  size_t readFrames = 0;
  {
    std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
    readFrames = audioRingBufferRead(&gAudio.state.m4aBuffer, out, frameCount,
                                     gAudio.state.channels);
  }
  if (framesRead) *framesRead = static_cast<uint64_t>(readFrames);
  gAudio.state.m4aCv.notify_one();
  return true;
}

bool totalM4aBackend(uint64_t* outFrames) {
  if (!outFrames) return false;
  *outFrames = gAudio.state.totalFrames.load(std::memory_order_relaxed);
  return true;
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
#ifdef _WIN32
  return ma_decoder_init_file_w(file.c_str(), &decConfig,
                                &gAudio.state.decoder) == MA_SUCCESS;
#else
  const std::string pathUtf8 = toUtf8String(file);
  return ma_decoder_init_file(pathUtf8.c_str(), &decConfig,
                              &gAudio.state.decoder) == MA_SUCCESS;
#endif
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
    AudioMode::M4a, false, false, false, true, initM4aBackend, uninitM4aBackend,
    readM4aBackend, nullptr, totalM4aBackend, nullptr};
const AudioBackendHandlers kBackendFfmpeg{
    AudioMode::Ffmpeg, false, true, true, true, initFfmpegBackend,
    uninitFfmpegBackend, readFfmpegBackend, seekFfmpegBackend,
    totalFfmpegBackend, nullptr};
const AudioBackendHandlers kBackendFlac{
    AudioMode::Flac, false, true, true, true, initFlacBackend, uninitFlacBackend,
    readFlacBackend, seekFlacBackend, totalFlacBackend, nullptr};
const AudioBackendHandlers kBackendKss{
    AudioMode::Kss, true, true, true, true, initKssBackend, uninitKssBackend,
    readKssBackend, seekKssBackend, totalKssBackend, nullptr};
const AudioBackendHandlers kBackendPsf{
    AudioMode::Psf, true, true, true, false, initPsfBackend, uninitPsfBackend,
    readPsfBackend, seekPsfBackend, totalPsfBackend, nullptr};
const AudioBackendHandlers kBackendGsf{
    AudioMode::Gsf, true, true, true, false, initGsfBackend, uninitGsfBackend,
    readGsfBackend, seekGsfBackend, totalGsfBackend, warningGsfBackend};
const AudioBackendHandlers kBackendVgm{
    AudioMode::Vgm, true, true, true, true, initVgmBackend, uninitVgmBackend,
    readVgmBackend, seekVgmBackend, totalVgmBackend, warningVgmBackend};
const AudioBackendHandlers kBackendGme{
    AudioMode::Gme, true, true, true, true, initGmeBackend, uninitGmeBackend,
    readGmeBackend, seekGmeBackend, totalGmeBackend, warningGmeBackend};
const AudioBackendHandlers kBackendMidi{
    AudioMode::Midi, false, true, true, true, initMidiBackend, uninitMidiBackend,
    readMidiBackend, seekMidiBackend, totalMidiBackend, nullptr};
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
  gAudio.state.streamBaseValid.store(false, std::memory_order_relaxed);
  gAudio.state.streamBasePtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.m4aAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.streamRb.reset();
  gAudio.state.audioClock.reset(0);
  clearQueuedPlaybackMetadata(&gAudio.state);
  gAudio.state.decodeCv.notify_all();
}

bool initDecoderForBackend(const AudioBackendHandlers* backend,
                           const std::filesystem::path& file,
                           uint64_t startFrame, int trackIndex) {
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
  activateBackend(backend, trackIndex);
  storeTotalFramesFromBackend(backend);
  return true;
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

void resetPlaybackStateForLoad(uint64_t startFrame) {
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
  gAudio.state.streamBaseValid.store(false, std::memory_order_relaxed);
  gAudio.state.streamBasePtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.m4aAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.deviceDelayFrames.store(0, std::memory_order_relaxed);

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

  audioPlaybackDeviceUninit();
  stopAndUninitActiveDecoder();

  if (!initDecoderForBackend(backend, file, startFrame, trackIndex)) {
    return false;
  }

  seekLoadedDecoderToStart(backend, &startFrame);
  resetPlaybackStateForLoad(startFrame);

  if (backendUsesQueuedDecodeThread(backend)) {
    const uint32_t rbFrames = std::max<uint32_t>(gAudio.sampleRate / 2, 8192);
    gAudio.state.streamRb.init(rbFrames, gAudio.channels);
    gAudio.state.streamQueueEnabled.store(true, std::memory_order_relaxed);
    resetQueuedPlaybackState(&gAudio.state, startFrame, 0);
    if (!startDecodeWorker(backend, startFrame)) {
      stopAndUninitActiveDecoder();
      return false;
    }
  }

  if (!audioPlaybackDeviceEnsureRunning()) {
    stopAndUninitActiveDecoder();
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
  audioPlaybackDeviceUninit();
  if (gAudio.decoderReady) {
    stopAndUninitActiveDecoder();
  }
  gAudio.state.framesPlayed.store(0);
  gAudio.state.audioClockFrames.store(0);
  gAudio.state.finished.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingSeekFrames.store(0);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.paused.store(false);
  gAudio.state.hold.store(false);
  gAudio.state.externalStream.store(false);
  gAudio.state.streamQueueEnabled.store(false);
  gAudio.state.streamSerial.store(0);
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
void audioStreamProcessRadio(float* interleaved, uint32_t frames) {
  audioStreamProcessRadioImpl(interleaved, frames);
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
  audioPlaybackDeviceUninit();
  stopAndUninitActiveDecoder();

  const uint32_t rbFrames = std::max<uint32_t>(gAudio.sampleRate, 8192);
  {
    std::lock_guard<std::mutex> lock(gAudio.state.m4aMutex);
    audioRingBufferInit(&gAudio.state.m4aBuffer, rbFrames, gAudio.channels);
    gAudio.state.m4aInitDone = true;
    gAudio.state.m4aInitOk = true;
    gAudio.state.m4aInitError.clear();
  }
  gAudio.state.streamRb.init(rbFrames, gAudio.channels);
  gAudio.state.streamQueueEnabled.store(true);
  gAudio.state.streamSerial.store(0);
  gAudio.state.streamBaseValid.store(false);
  gAudio.state.streamBasePtsUs.store(0);
  gAudio.state.streamReadFrames.store(0);
  gAudio.state.streamClockReady.store(false);
  gAudio.state.streamStarved.store(false);
  gAudio.state.audioClock.reset(0);
  gAudio.state.m4aThreadRunning.store(false);
  gAudio.state.m4aStop.store(false);
  gAudio.state.m4aAtEnd.store(false);
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

  gAudio.state.channels = gAudio.channels;
  gAudio.state.sampleRate = gAudio.sampleRate;
  rebuildRadioPreviewChain(&gAudio.state);

  if (!audioPlaybackDeviceEnsureRunning()) {
    stopAndUninitActiveDecoder();
    gAudio.state.streamQueueEnabled.store(false);
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

bool audioStreamWriteSamples(const float* interleaved, uint64_t frames,
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

uint64_t audioStreamDropFrames(uint64_t frames) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return 0;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return 0;
  }
  return gAudio.state.streamRb.dropSome(frames);
}

void audioStreamSynchronize(int serial, int64_t targetPtsUs) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load() ||
      !gAudio.state.streamQueueEnabled.load()) {
    return;
  }

  // We dwingen een harde synchronisatie.
  // Door de ringbuffer te resetten, zorgen we ervoor dat 'oude' audio direct
  // wordt weggegooid en de klok onmiddellijk op de nieuwe tijd staat.
  gAudio.state.streamRb.reset();
  
  {
    std::lock_guard<std::mutex> lock(gAudio.state.streamMetadataMutex);
    gAudio.state.streamMetadata.clear();
    // We voegen de nieuwe anker-pts toe voor de EERSTE sample die nu geschreven gaat worden (wpos=0).
    gAudio.state.streamMetadata.push_back({
        0,
        targetPtsUs,
        serial
    });
  }

  gAudio.state.streamDiscardPtsUs.store(targetPtsUs, std::memory_order_relaxed);
  gAudio.state.streamBasePtsUs.store(targetPtsUs, std::memory_order_relaxed);
  gAudio.state.audioClock.set(targetPtsUs, nowUs(), serial);
  gAudio.state.streamClockReady.store(true, std::memory_order_relaxed);
  gAudio.state.decodeCv.notify_all();
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

void audioStreamSyncClockOnly(int serial, int64_t targetPtsUs) {
  // Soft sync: only adjust the clock, don't reset ringbuffer or drop audio
  // Used when video PTS is repaired to prevent audio clock from becoming invalid
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load() ||
      !gAudio.state.streamQueueEnabled.load()) {
    return;
  }
  
  // Just resync the clock without touching the ringbuffer
  // This keeps audio playing smoothly while video catches up
  gAudio.state.audioClock.sync_to_pts(targetPtsUs, nowUs(), serial);
}

void audioStreamSetBase(int serial, int64_t ptsUs) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return;
  }
  if (serial != gAudio.state.streamSerial.load(std::memory_order_relaxed)) {
    return;
  }
  gAudio.state.streamBasePtsUs.store(ptsUs, std::memory_order_relaxed);
  gAudio.state.streamBaseValid.store(true, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.audioClock.reset(serial);
}

void audioStreamDiscardUntil(int64_t ptsUs) {
  gAudio.state.streamDiscardPtsUs.store(ptsUs, std::memory_order_relaxed);
}

void audioStreamSetEnd(bool atEnd) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  gAudio.state.m4aAtEnd.store(atEnd);
}

void audioStreamReset(uint64_t framePos) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  gAudio.state.streamRb.reset();
  gAudio.state.framesPlayed.store(framePos);
  gAudio.state.audioClockFrames.store(framePos);
  gAudio.state.finished.store(false);
  gAudio.state.m4aAtEnd.store(false);
  gAudio.state.seekRequested.store(false);
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
  gAudio.state.decodeCv.notify_all();
}

void audioStreamFlushSerial(int serial) {
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return;
  }
  gAudio.state.streamSerial.store(serial, std::memory_order_relaxed);
  gAudio.state.streamRb.reset();
  gAudio.state.streamBaseValid.store(false, std::memory_order_relaxed);
  gAudio.state.streamBasePtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.m4aAtEnd.store(false);
  gAudio.state.finished.store(false);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.audioClock.reset(serial);
  {
    std::lock_guard<std::mutex> lock(gAudio.state.streamMetadataMutex);
    gAudio.state.streamMetadata.clear();
  }
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
  const bool finished = gAudio.state.finished.load(std::memory_order_relaxed);
  const bool externalStream =
      gAudio.state.externalStream.load(std::memory_order_relaxed);
  if (finished && !externalStream && !gAudio.nowPlaying.empty()) {
    loadFileAt(gAudio.nowPlaying, 0, gAudio.trackIndex);
    return;
  }
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
  gAudio.state.m4aAtEnd.store(false, std::memory_order_relaxed);
  if (gAudio.state.externalStream.load(std::memory_order_relaxed)) {
    gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
    gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
    gAudio.state.audioClock.reset(
        gAudio.state.streamSerial.load(std::memory_order_relaxed));
  }
  gAudio.state.paused.store(false, std::memory_order_relaxed);
}

void audioSeekBy(int direction) {
  if (!gAudio.decoderReady) return;
  int64_t deltaFrames = static_cast<int64_t>(direction) * 5 * gAudio.sampleRate;
  int64_t current = static_cast<int64_t>(gAudio.state.audioClockFrames.load());
  int64_t target = current + deltaFrames;
  if (target < 0) target = 0;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total > 0 && target > static_cast<int64_t>(total)) {
    target = static_cast<int64_t>(total);
  }
  gAudio.state.pendingSeekFrames.store(target);
  gAudio.state.seekRequested.store(true);
  gAudio.state.finished.store(false);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.m4aCv.notify_all();
  gAudio.state.decodeCv.notify_all();
}

void audioSeekToRatio(double ratio) {
  if (!gAudio.decoderReady) return;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total == 0) return;
  ratio = std::clamp(ratio, 0.0, 1.0);
  int64_t target = static_cast<int64_t>(ratio * static_cast<double>(total));
  gAudio.state.pendingSeekFrames.store(target);
  gAudio.state.seekRequested.store(true);
  gAudio.state.finished.store(false);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.m4aCv.notify_all();
  gAudio.state.decodeCv.notify_all();
}

void audioSeekToSec(double sec) {
  if (!gAudio.decoderReady) return;
  if (!std::isfinite(sec)) return;
  int64_t target =
      static_cast<int64_t>(std::llround(sec * gAudio.sampleRate));
  if (target < 0) target = 0;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total > 0 && static_cast<uint64_t>(target) > total) {
    target = static_cast<int64_t>(total);
  }
  gAudio.state.pendingSeekFrames.store(target);
  gAudio.state.seekRequested.store(true);
  gAudio.state.finished.store(false);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.m4aCv.notify_all();
  gAudio.state.decodeCv.notify_all();
}

void audioToggleRadio() {
  bool next = !gAudio.state.useRadio1938.load();
  gAudio.state.useRadio1938.store(next);
  if (next) {
    applyRadioTogglePreset();
  }
}

void audioSetHold(bool hold) { gAudio.state.hold.store(hold); }

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
