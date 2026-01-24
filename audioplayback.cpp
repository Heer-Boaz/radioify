#include "audioplayback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavutil/avutil.h>
}

#include "clock.h"
#include "ffmpegaudio.h"
#include "gmeaudio.h"
#include "kssaudio.h"
#include "radio.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define MINIAUDIO_IMPLEMENTATION
#define MA_ENABLE_WAV
#define MA_ENABLE_MP3
#define MA_ENABLE_FLAC
#include "miniaudio.h"
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif

#include "timing_log.h"

namespace {
std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

static std::string toUtf8String(const std::filesystem::path& p) {
#ifdef _WIN32
  auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
#else
  return p.string();
#endif
}

int64_t nowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool isSupportedAudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".m4a" ||
         ext == ".webm" || ext == ".mp4" || ext == ".kss" || ext == ".nsf";
}

bool isMiniaudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

bool isGmeExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".nsf";
}

bool isM4aExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".m4a" || ext == ".mp4" || ext == ".webm";
}

bool isKssExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".kss";
}

void validateInputFile(const std::filesystem::path& p) {
  if (p.empty()) return;
  if (!std::filesystem::exists(p)) return;
  if (std::filesystem::is_directory(p)) return;
  if (!isSupportedAudioExt(p)) return;
}

struct AudioRingBuffer {
  std::vector<float> data;
  size_t capacityFrames = 0;
  size_t readPos = 0;
  size_t writePos = 0;
  size_t bufferedFrames = 0;
};

struct AudioSampleRing {
  std::vector<float> buf;
  uint32_t channels = 0;
  uint64_t capFrames = 0;
  std::atomic<uint64_t> rpos{0};
  std::atomic<uint64_t> wpos{0};

  void init(uint64_t capacityFrames, uint32_t ch) {
    channels = ch;
    capFrames = capacityFrames;
    buf.assign(capFrames * channels, 0.0f);
    rpos.store(0, std::memory_order_relaxed);
    wpos.store(0, std::memory_order_relaxed);
  }

  void reset() {
    rpos.store(0, std::memory_order_release);
    wpos.store(0, std::memory_order_release);
  }

  uint64_t bufferedFrames() const {
    uint64_t w = wpos.load(std::memory_order_acquire);
    uint64_t r = rpos.load(std::memory_order_acquire);
    return w - r;
  }

  uint64_t writableFrames() const {
    return capFrames - bufferedFrames();
  }

  uint64_t writeSome(const float* in, uint64_t frames) {
    uint64_t w = wpos.load(std::memory_order_relaxed);
    uint64_t r = rpos.load(std::memory_order_acquire);
    uint64_t avail = capFrames - (w - r);
    if (avail == 0) return 0;
    uint64_t n = (frames < avail) ? frames : avail;

    uint64_t wi = w % capFrames;
    uint64_t first = std::min<uint64_t>(n, capFrames - wi);

    std::memcpy(buf.data() + wi * channels, in,
                first * channels * sizeof(float));
    if (n > first) {
      std::memcpy(buf.data(), in + first * channels,
                  (n - first) * channels * sizeof(float));
    }

    wpos.store(w + n, std::memory_order_release);
    return n;
  }

  uint64_t readSome(float* out, uint64_t frames) {
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

  uint64_t dropSome(uint64_t frames) {
    uint64_t r = rpos.load(std::memory_order_relaxed);
    uint64_t w = wpos.load(std::memory_order_acquire);
    uint64_t avail = w - r;
    if (avail == 0) return 0;
    uint64_t n = (frames < avail) ? frames : avail;
    rpos.store(r + n, std::memory_order_release);
    return n;
  }
};

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

uint64_t rescaleFrames(uint64_t frames, uint32_t inRate, uint32_t outRate) {
  if (frames == 0 || inRate == 0 || outRate == 0 || inRate == outRate) {
    return frames;
  }
  double scaled = static_cast<double>(frames) * static_cast<double>(outRate) /
                  static_cast<double>(inRate);
  return static_cast<uint64_t>(std::llround(scaled));
}

struct AudioMetadata {
  uint64_t wpos;      // The write position (wpos) when this packet was added
  int64_t ptsUs;      // The PTS of the first sample in this packet
  int serial;         // The serial of this packet
};

struct AudioState {
  ma_decoder decoder{};
  GmeAudioDecoder gme{};
  KssAudioDecoder kss{};
  ma_device device{};
  Radio1938 radio1938{};
  AudioRingBuffer m4aBuffer;
  std::mutex m4aMutex;
  std::condition_variable m4aCv;
  std::thread m4aThread;
  bool m4aInitDone = false;
  bool m4aInitOk = false;
  std::string m4aInitError;
  std::atomic<bool> m4aThreadRunning{false};
  std::atomic<bool> m4aStop{false};
  std::atomic<bool> m4aAtEnd{false};
  std::atomic<bool> externalStream{false};
  std::atomic<bool> paused{false};
  std::atomic<bool> hold{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> useRadio1938{true};
  std::atomic<bool> usingFfmpeg{false};
  std::atomic<bool> usingGme{false};
  std::atomic<bool> usingKss{false};
  std::atomic<bool> seekRequested{false};
  std::atomic<int64_t> pendingSeekFrames{0};
  std::atomic<uint64_t> framesPlayed{0};
  std::atomic<uint64_t> callbackCount{0};
  std::atomic<uint64_t> framesRequested{0};
  std::atomic<uint64_t> framesReadTotal{0};
  std::atomic<uint64_t> shortReadCount{0};
  std::atomic<uint64_t> silentFrames{0};
  std::atomic<uint64_t> pausedCallbacks{0};
  std::atomic<uint32_t> lastCallbackFrames{0};
  std::atomic<uint32_t> lastFramesRead{0};
  std::atomic<uint64_t> audioClockFrames{0};
  std::atomic<uint64_t> totalFrames{0};
  std::atomic<uint64_t> audioPaddingFrames{0};
  std::atomic<uint64_t> audioTrailingPaddingFrames{0};
  std::atomic<uint64_t> audioLeadSilenceFrames{0};
  std::atomic<bool> audioPrimed{false};
  std::atomic<float> volume{1.0f};
  AudioSampleRing streamRb;
  std::atomic<bool> streamQueueEnabled{false};
  std::atomic<int> streamSerial{0};
  std::atomic<bool> streamBaseValid{false};
  std::atomic<int64_t> streamBasePtsUs{0};
  std::atomic<uint64_t> streamReadFrames{0};
  std::atomic<bool> streamClockReady{false};

  std::deque<AudioMetadata> streamMetadata;
  std::mutex streamMetadataMutex;
  std::condition_variable streamUpdateCv;
  std::atomic<uint64_t> streamUpdateCounter{0};

  std::atomic<bool> streamStarved{false};
  std::atomic<int64_t> streamDiscardPtsUs{0};
  uint64_t deviceDelayFrames = 0;
  Clock audioClock;
  uint32_t channels = 1;
  uint32_t sampleRate = 48000;
  bool dry = false;
};

struct AudioPlaybackState {
  AudioState state{};
  Radio1938 radio1938Template{};
  bool deviceReady = false;
  bool decoderReady = false;
  bool enableAudio = false;
  bool dry = false;
  bool enableRadio = false;
  uint32_t sampleRate = 48000;
  uint32_t baseChannels = 1;
  uint32_t channels = 1;
  float lpHz = 0.0f;
  float noise = 0.0f;
  std::filesystem::path nowPlaying;
  int gmeTrackIndex = 0;
  std::string gmeWarning;
  int kssTrackIndex = 0;
  KssPlaybackOptions kssOptions{};
};

AudioPlaybackState gAudio;

void dataCallback(ma_device* device, void* output, const void*,
                  ma_uint32 frameCount) {
  auto* state = static_cast<AudioState*>(device->pUserData);
  float* out = static_cast<float*>(output);
  if (!state) return;

  state->callbackCount.fetch_add(1, std::memory_order_relaxed);
  state->framesRequested.fetch_add(frameCount, std::memory_order_relaxed);
  state->lastCallbackFrames.store(frameCount, std::memory_order_relaxed);
  state->audioPrimed.store(true, std::memory_order_relaxed);

  bool useStreamQueue =
      state->externalStream.load() && state->streamQueueEnabled.load();
  if (useStreamQueue) {
    const uint32_t channels = state->channels;
    const bool paused = state->paused.load() || state->hold.load();
    if (paused) {
      state->pausedCallbacks.fetch_add(1, std::memory_order_relaxed);
      state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
      state->lastFramesRead.store(0, std::memory_order_relaxed);
      std::fill(out, out + frameCount * channels, 0.0f);
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

    if (!state->dry && framesRead > 0 && state->useRadio1938.load()) {
      state->radio1938.process(out, static_cast<uint32_t>(framesRead));
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
        uint64_t delay = state->deviceDelayFrames;
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
    // We allow a small "free-run" window (150ms) to bridge decoder hiccups 
    // without freezing the video clock. This prevents micro-stutters.
    if (starved) {
      int64_t now = nowUs();
      int64_t last = state->audioClock.last_updated_us.load(std::memory_order_relaxed);
      if (now - last > 150000) { // 150ms threshold
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
    if (vol != 1.0f) {
      for (ma_uint32 i = 0; i < frameCount * channels; ++i) {
        float x = out[i] * vol;
        if (vol > 1.0f) {
          if (x > 0.9f) {
            float r = x - 0.9f;
            x = 0.9f + 0.1f * (r / (0.1f + r));
          } else if (x < -0.9f) {
            float r = -x - 0.9f;
            x = -(0.9f + 0.1f * (r / (0.1f + r)));
          }
        }
        out[i] = x;
      }
    }
    return;
  }

  if (state->hold.load()) {
    state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    std::fill(out, out + frameCount * state->channels, 0.0f);
    return;
  }

  bool usingFfmpeg = state->usingFfmpeg.load();
  bool usingGme = state->usingGme.load();
  bool usingKss = state->usingKss.load();
  if (!usingFfmpeg && state->seekRequested.exchange(false)) {
    int64_t target = state->pendingSeekFrames.load();
    if (target < 0) target = 0;
    if (usingKss) {
      if (!state->kss.seekToFrame(static_cast<uint64_t>(target))) {
        target = 0;
        state->kss.seekToFrame(0);
      }
    } else if (usingGme) {
      if (!state->gme.seekToFrame(static_cast<uint64_t>(target))) {
        target = 0;
        state->gme.seekToFrame(0);
      }
    } else {
      ma_decoder_seek_to_pcm_frame(&state->decoder,
                                   static_cast<ma_uint64>(target));
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
  if (framesRemaining > 0) {
    if (usingFfmpeg) {
      if (!state->externalStream.load() && state->seekRequested.load()) {
        std::lock_guard<std::mutex> lock(state->m4aMutex);
        audioRingBufferReset(&state->m4aBuffer);
        state->m4aAtEnd.store(false);
        state->finished.store(false);
      }
      size_t readFrames = 0;
      {
        std::lock_guard<std::mutex> lock(state->m4aMutex);
        readFrames = audioRingBufferRead(&state->m4aBuffer, outCursor,
                                         framesRemaining, state->channels);
      }
      framesRead = readFrames;
      if (readFrames < framesRemaining) {
        std::fill(outCursor + readFrames * state->channels,
                  outCursor + framesRemaining * state->channels, 0.0f);
      }
      if (readFrames == 0 && state->m4aAtEnd.load()) {
        state->finished.store(true);
        uint64_t total = state->totalFrames.load();
        if (total > 0) {
          state->framesPlayed.store(total);
        }
      }
      state->m4aCv.notify_one();
    } else if (usingKss) {
      uint64_t framesReadKss = 0;
      if (!state->kss.readFrames(outCursor, framesRemaining, &framesReadKss)) {
        state->finished.store(true);
        return;
      }
      framesRead = framesReadKss;
      if (framesRead < framesRemaining) {
        std::fill(outCursor + framesRead * state->channels,
                  outCursor + framesRemaining * state->channels, 0.0f);
      }
      if (framesRead == 0 || framesRead < framesRemaining) {
        state->finished.store(true);
        uint64_t total = state->totalFrames.load();
        if (total > 0) {
          state->framesPlayed.store(total);
        }
      }
    } else if (usingGme) {
      uint64_t framesReadGme = 0;
      if (!state->gme.readFrames(outCursor, framesRemaining, &framesReadGme)) {
        state->finished.store(true);
        return;
      }
      framesRead = framesReadGme;
      if (framesRead < framesRemaining) {
        std::fill(outCursor + framesRead * state->channels,
                  outCursor + framesRemaining * state->channels, 0.0f);
      }
      if (framesRead == 0 || framesRead < framesRemaining) {
        state->finished.store(true);
        uint64_t total = state->totalFrames.load();
        if (total > 0) {
          state->framesPlayed.store(total);
        }
      }
    } else {
      ma_uint64 framesReadMa = 0;
      ma_result res = ma_decoder_read_pcm_frames(
          &state->decoder, outCursor, framesRemaining, &framesReadMa);
      if (res != MA_SUCCESS && res != MA_AT_END) {
        state->finished.store(true);
        return;
      }
      framesRead = framesReadMa;
      if (framesRead < framesRemaining) {
        std::fill(outCursor + framesRead * state->channels,
                  outCursor + framesRemaining * state->channels, 0.0f);
      }
      if (res == MA_AT_END || framesRead == 0) {
        state->finished.store(true);
        uint64_t total = state->totalFrames.load();
        if (total > 0) {
          state->framesPlayed.store(total);
        }
      }
    }
  }

  state->audioClockFrames.fetch_add(frameCount, std::memory_order_relaxed);
  if (!state->dry && framesRead > 0 && state->useRadio1938.load()) {
    float* audioStart =
        out + silentLeadFrames * static_cast<uint64_t>(state->channels);
    state->radio1938.process(audioStart, static_cast<uint32_t>(framesRead));
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
  if (vol != 1.0f) {
    for (ma_uint32 i = 0; i < frameCount * state->channels; ++i) {
      float x = out[i] * vol;
      if (vol > 1.0f) {
        if (x > 0.9f) {
          float r = x - 0.9f;
          x = 0.9f + 0.1f * (r / (0.1f + r));
        } else if (x < -0.9f) {
          float r = -x - 0.9f;
          x = -(0.9f + 0.1f * (r / (0.1f + r)));
        }
      }
      out[i] = x;
    }
  }
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
    auto t_init_start = std::chrono::steady_clock::now();
    bool ok = decoder.init(file, workerChannels, workerRate, &initError);
    auto t_init_end = std::chrono::steady_clock::now();
#if RADIOIFY_ENABLE_TIMING_LOG
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
      {
        std::ofstream f((std::filesystem::current_path() /
                         "radioify.log")
                            .string(),
                        std::ios::app);
        if (f) f << radioifyLogTimestamp() << " " << buf << "\n";
      }
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
        uint64_t targetFrames = static_cast<uint64_t>(target);
        uint64_t rawTarget = targetFrames + paddingFrames;
        if (rawTotalFrames > 0 && rawTarget > rawTotalFrames) {
          rawTarget = rawTotalFrames;
        }
        if (!decoder.seekToFrame(rawTarget)) {
          rawTarget = 0;
          targetFrames = 0;
          decoder.seekToFrame(0);
        }
        gAudio.state.framesPlayed.store(targetFrames);
        gAudio.state.audioClockFrames.store(targetFrames);
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
          std::ofstream f((std::filesystem::current_path() /
                           "radioify.log")
                              .string(),
                          std::ios::app);
          if (f) f << radioifyLogTimestamp() << " " << buf << "\n";
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

bool initDevice() {
  if (gAudio.deviceReady) return true;
  ma_device_config devConfig = ma_device_config_init(ma_device_type_playback);
  devConfig.playback.format = ma_format_f32;
  devConfig.playback.channels = gAudio.channels;
  devConfig.sampleRate = gAudio.sampleRate;
  devConfig.dataCallback = dataCallback;
  devConfig.pUserData = &gAudio.state;

  if (ma_device_init(nullptr, &devConfig, &gAudio.state.device) != MA_SUCCESS) {
    return false;
  }
  if (ma_device_start(&gAudio.state.device) != MA_SUCCESS) {
    ma_device_uninit(&gAudio.state.device);
    return false;
  }
  gAudio.deviceReady = true;
#if RADIOIFY_ENABLE_TIMING_LOG
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "audio_device_started sampleRate=%u channels=%u",
                  gAudio.sampleRate, gAudio.channels);
    std::ofstream f((std::filesystem::current_path() / "radioify.log")
                        .string(),
                    std::ios::app);
    if (f) f << radioifyLogTimestamp() << " " << buf << "\n";
  }
#endif
  return true;
}

bool loadFileAt(const std::filesystem::path& file, uint64_t startFrame,
                int trackIndex) {
  if (!gAudio.enableAudio) {
    return false;
  }
  validateInputFile(file);
  gAudio.gmeWarning.clear();
  const bool useM4a = isM4aExt(file);
  const bool useMiniaudio = isMiniaudioExt(file);
  const bool useGme = isGmeExt(file);
  const bool useKss = isKssExt(file);
  if (!useM4a && !useMiniaudio && !useGme && !useKss) {
    return false;
  }
  if (gAudio.deviceReady) {
    ma_device_stop(&gAudio.state.device);
  }
  if (gAudio.decoderReady) {
    if (gAudio.state.usingFfmpeg.load()) {
      stopM4aWorker();
    } else if (gAudio.state.usingKss.load()) {
      gAudio.state.kss.uninit();
    } else if (gAudio.state.usingGme.load()) {
      gAudio.state.gme.uninit();
    } else {
      ma_decoder_uninit(&gAudio.state.decoder);
    }
    gAudio.decoderReady = false;
    gAudio.state.usingFfmpeg.store(false);
    gAudio.state.usingGme.store(false);
    gAudio.state.usingKss.store(false);
    gAudio.state.externalStream.store(false);
  }

  if (useM4a) {
    std::string error;
    gAudio.state.totalFrames.store(0);
    if (!startM4aWorker(file, startFrame, &error)) {
      return false;
    }
    gAudio.decoderReady = true;
    gAudio.state.usingFfmpeg.store(true);
    gAudio.state.usingGme.store(false);
    gAudio.state.usingKss.store(false);
    gAudio.state.externalStream.store(false);
    gAudio.gmeTrackIndex = 0;
    gAudio.kssTrackIndex = 0;
  } else if (useKss) {
    std::string error;
    if (!gAudio.state.kss.init(file, gAudio.channels, gAudio.sampleRate,
                               &error, trackIndex, gAudio.kssOptions)) {
      return false;
    }
    gAudio.decoderReady = true;
    gAudio.state.usingFfmpeg.store(false);
    gAudio.state.usingGme.store(false);
    gAudio.state.usingKss.store(true);
    gAudio.state.externalStream.store(false);
    gAudio.kssTrackIndex = trackIndex;
    gAudio.gmeTrackIndex = 0;

    uint64_t totalFrames = 0;
    if (gAudio.state.kss.getTotalFrames(&totalFrames)) {
      gAudio.state.totalFrames.store(totalFrames);
    } else {
      gAudio.state.totalFrames.store(0);
    }
  } else if (useGme) {
    std::string error;
    if (!gAudio.state.gme.init(file, gAudio.channels, gAudio.sampleRate,
                               &error, trackIndex)) {
      return false;
    }
    gAudio.decoderReady = true;
    gAudio.state.usingFfmpeg.store(false);
    gAudio.state.usingGme.store(true);
    gAudio.state.usingKss.store(false);
    gAudio.state.externalStream.store(false);
    gAudio.gmeTrackIndex = trackIndex;
    gAudio.kssTrackIndex = 0;
    gAudio.gmeWarning = gAudio.state.gme.warning();

    uint64_t totalFrames = 0;
    if (gAudio.state.gme.getTotalFrames(&totalFrames)) {
      gAudio.state.totalFrames.store(totalFrames);
    } else {
      gAudio.state.totalFrames.store(0);
    }
  } else {
    if (!useMiniaudio) {
      return false;
    }
    ma_decoder_config decConfig =
        ma_decoder_config_init(ma_format_f32, gAudio.channels,
                               gAudio.sampleRate);
    if (ma_decoder_init_file(file.string().c_str(), &decConfig,
                             &gAudio.state.decoder) != MA_SUCCESS) {
      return false;
    }
    gAudio.decoderReady = true;
    gAudio.state.usingFfmpeg.store(false);
    gAudio.state.usingGme.store(false);
    gAudio.state.usingKss.store(false);
    gAudio.state.externalStream.store(false);
    gAudio.gmeTrackIndex = 0;
    gAudio.kssTrackIndex = 0;

    ma_uint64 totalFrames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&gAudio.state.decoder,
                                            &totalFrames) == MA_SUCCESS) {
      gAudio.state.totalFrames.store(totalFrames);
    } else {
      gAudio.state.totalFrames.store(0);
    }
  }

  if (useKss) {
    uint64_t total = gAudio.state.totalFrames.load();
    if (total > 0 && startFrame > total) {
      startFrame = total;
    }
    if (startFrame > 0) {
      if (!gAudio.state.kss.seekToFrame(startFrame)) {
        startFrame = 0;
        gAudio.state.kss.seekToFrame(0);
      }
    }
  } else if (useGme) {
    uint64_t total = gAudio.state.totalFrames.load();
    if (total > 0 && startFrame > total) {
      startFrame = total;
    }
    if (startFrame > 0) {
      if (!gAudio.state.gme.seekToFrame(startFrame)) {
        startFrame = 0;
        gAudio.state.gme.seekToFrame(0);
      }
    }
  } else if (!useM4a) {
    uint64_t total = gAudio.state.totalFrames.load();
    if (total > 0 && startFrame > total) {
      startFrame = total;
    }
    if (startFrame > 0) {
      if (ma_decoder_seek_to_pcm_frame(
              &gAudio.state.decoder, static_cast<ma_uint64>(startFrame)) !=
          MA_SUCCESS) {
        startFrame = 0;
      }
    }
  }

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

  gAudio.state.channels = gAudio.channels;
  gAudio.state.radio1938 = gAudio.radio1938Template;
  gAudio.state.radio1938.init(gAudio.channels,
                              static_cast<float>(gAudio.sampleRate),
                              gAudio.lpHz, gAudio.noise);

  if (!gAudio.deviceReady) {
    if (!initDevice()) {
      if (gAudio.state.usingFfmpeg.load()) {
        stopM4aWorker();
      } else if (gAudio.state.usingKss.load()) {
        gAudio.state.kss.uninit();
      } else if (gAudio.state.usingGme.load()) {
        gAudio.state.gme.uninit();
      } else {
        ma_decoder_uninit(&gAudio.state.decoder);
      }
      gAudio.decoderReady = false;
      return false;
    }
  } else {
    if (ma_device_start(&gAudio.state.device) != MA_SUCCESS) {
      if (gAudio.state.usingFfmpeg.load()) {
        stopM4aWorker();
      } else if (gAudio.state.usingKss.load()) {
        gAudio.state.kss.uninit();
      } else if (gAudio.state.usingGme.load()) {
        gAudio.state.gme.uninit();
      } else {
        ma_decoder_uninit(&gAudio.state.decoder);
      }
      gAudio.decoderReady = false;
      return false;
    }
  }

  gAudio.nowPlaying = file;
  return true;
}

bool loadFile(const std::filesystem::path& file, int trackIndex) {
  return loadFileAt(file, 0, trackIndex);
}

bool ensureChannels(uint32_t newChannels) {
  if (newChannels == gAudio.channels) return true;

  uint64_t resumeFrame = gAudio.decoderReady ? gAudio.state.framesPlayed.load()
                                             : 0;
  bool hadTrack = gAudio.decoderReady && !gAudio.nowPlaying.empty();
  bool wasKss = gAudio.state.usingKss.load();
  int trackIndex = wasKss ? gAudio.kssTrackIndex : gAudio.gmeTrackIndex;

  if (gAudio.deviceReady) {
    ma_device_stop(&gAudio.state.device);
    ma_device_uninit(&gAudio.state.device);
    gAudio.deviceReady = false;
  }
  if (gAudio.decoderReady) {
    if (gAudio.state.usingFfmpeg.load()) {
      stopM4aWorker();
    } else if (gAudio.state.usingKss.load()) {
      gAudio.state.kss.uninit();
    } else if (gAudio.state.usingGme.load()) {
      gAudio.state.gme.uninit();
    } else {
      ma_decoder_uninit(&gAudio.state.decoder);
    }
    gAudio.decoderReady = false;
    gAudio.state.usingFfmpeg.store(false);
    gAudio.state.usingGme.store(false);
    gAudio.state.usingKss.store(false);
  }

  gAudio.channels = newChannels;
  gAudio.state.channels = gAudio.channels;
  gAudio.radio1938Template.init(gAudio.channels,
                                static_cast<float>(gAudio.sampleRate),
                                gAudio.lpHz,
                                gAudio.noise);
  gAudio.state.radio1938 = gAudio.radio1938Template;

  if (hadTrack) {
    return loadFileAt(gAudio.nowPlaying, resumeFrame, trackIndex);
  }
  return true;
}

void stopPlayback() {
  if (gAudio.deviceReady) {
    ma_device_stop(&gAudio.state.device);
  }
  if (gAudio.decoderReady) {
    if (gAudio.state.usingFfmpeg.load()) {
      stopM4aWorker();
    } else if (gAudio.state.usingKss.load()) {
      gAudio.state.kss.uninit();
    } else if (gAudio.state.usingGme.load()) {
      gAudio.state.gme.uninit();
    } else {
      ma_decoder_uninit(&gAudio.state.decoder);
    }
    gAudio.decoderReady = false;
    gAudio.state.usingFfmpeg.store(false);
    gAudio.state.usingGme.store(false);
    gAudio.state.usingKss.store(false);
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
  gAudio.nowPlaying.clear();
  gAudio.gmeTrackIndex = 0;
  gAudio.gmeWarning.clear();
  gAudio.kssTrackIndex = 0;
}
}  // namespace

void audioInit(const AudioPlaybackConfig& config) {
  gAudio.enableAudio = config.enableAudio;
  gAudio.enableRadio = config.enableRadio;
  gAudio.dry = config.dry;
  gAudio.sampleRate = 48000;
  gAudio.baseChannels = config.mono ? 1u : 2u;
  gAudio.channels = gAudio.baseChannels;
  gAudio.lpHz = static_cast<float>(config.bwHz);
  gAudio.noise = static_cast<float>(config.noise);

  gAudio.state.channels = gAudio.channels;
  gAudio.state.sampleRate = gAudio.sampleRate;
  gAudio.state.dry = config.dry;
  gAudio.state.useRadio1938.store(config.enableRadio);

  gAudio.radio1938Template.init(gAudio.channels,
                                static_cast<float>(gAudio.sampleRate),
                                gAudio.lpHz,
                                gAudio.noise);
  gAudio.state.radio1938 = gAudio.radio1938Template;
}

void audioShutdown() {
  stopPlayback();
  if (gAudio.deviceReady) {
    ma_device_uninit(&gAudio.state.device);
    gAudio.deviceReady = false;
  }
}

bool audioIsEnabled() { return gAudio.enableAudio; }

bool audioIsReady() { return gAudio.decoderReady; }

bool audioStartFile(const std::filesystem::path& file, int trackIndex) {
  if (!gAudio.enableAudio) return false;
  return loadFile(file, trackIndex);
}

bool audioStartFileAt(const std::filesystem::path& file, double startSec,
                      int trackIndex) {
  if (!gAudio.enableAudio) return false;
  if (!std::isfinite(startSec) || startSec <= 0.0) {
    return loadFile(file, trackIndex);
  }
  double framesDouble = startSec * static_cast<double>(gAudio.sampleRate);
  int64_t startFrame = static_cast<int64_t>(std::llround(framesDouble));
  if (startFrame < 0) startFrame = 0;
  return loadFileAt(file, static_cast<uint64_t>(startFrame), trackIndex);
}

void audioStop() { stopPlayback(); }

bool audioStartStream(uint64_t totalFrames) {
  if (!gAudio.enableAudio) {
    return false;
  }
  gAudio.gmeWarning.clear();
  gAudio.kssTrackIndex = 0;
  if (gAudio.deviceReady) {
    ma_device_stop(&gAudio.state.device);
  }
  if (gAudio.decoderReady) {
    if (gAudio.state.usingFfmpeg.load()) {
      stopM4aWorker();
    } else if (gAudio.state.usingKss.load()) {
      gAudio.state.kss.uninit();
    } else if (gAudio.state.usingGme.load()) {
      gAudio.state.gme.uninit();
    } else {
      ma_decoder_uninit(&gAudio.state.decoder);
    }
    gAudio.decoderReady = false;
    gAudio.state.usingFfmpeg.store(false);
    gAudio.state.usingGme.store(false);
    gAudio.state.usingKss.store(false);
  }

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
  gAudio.state.usingFfmpeg.store(true);
  gAudio.state.usingGme.store(false);
  gAudio.state.usingKss.store(false);
  gAudio.decoderReady = true;
  gAudio.gmeTrackIndex = 0;
  gAudio.kssTrackIndex = 0;

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
  gAudio.state.radio1938 = gAudio.radio1938Template;
  gAudio.state.radio1938.init(gAudio.channels,
                              static_cast<float>(gAudio.sampleRate),
                              gAudio.lpHz, gAudio.noise);

  if (!gAudio.deviceReady) {
    if (!initDevice()) {
      gAudio.decoderReady = false;
      gAudio.state.usingFfmpeg.store(false);
      gAudio.state.usingGme.store(false);
      gAudio.state.usingKss.store(false);
      gAudio.state.externalStream.store(false);
      gAudio.state.streamQueueEnabled.store(false);
      return false;
    }
  } else {
    if (ma_device_start(&gAudio.state.device) != MA_SUCCESS) {
      gAudio.decoderReady = false;
      gAudio.state.usingFfmpeg.store(false);
      gAudio.state.usingGme.store(false);
      gAudio.state.usingKss.store(false);
      gAudio.state.externalStream.store(false);
      gAudio.state.streamQueueEnabled.store(false);
      return false;
    }
  }

  // Estimate output latency so the audio clock tracks what is actually audible,
  // not what we just wrote into the callback buffer.
  uint64_t latencyFrames = 0;
  if (gAudio.deviceReady &&
      ma_device_get_state(&gAudio.state.device) !=
          ma_device_state_uninitialized) {
    uint32_t internalRate = gAudio.state.device.playback.internalSampleRate;
    uint64_t internalBuffer = 0;
    uint32_t periodFrames = gAudio.state.device.playback.internalPeriodSizeInFrames;
    uint32_t periods = gAudio.state.device.playback.internalPeriods;
    if (periodFrames > 0 && periods > 0) {
      internalBuffer = static_cast<uint64_t>(periodFrames) *
                       static_cast<uint64_t>(periods);
    }
#ifdef MA_SUPPORT_WASAPI
    if (gAudio.state.device.wasapi.actualBufferSizeInFramesPlayback > 0) {
      internalBuffer = std::max<uint64_t>(
          internalBuffer, gAudio.state.device.wasapi.actualBufferSizeInFramesPlayback);
    }
#endif
    if (internalBuffer > 0) {
      latencyFrames +=
          rescaleFrames(internalBuffer, internalRate, gAudio.state.sampleRate);
    }
    latencyFrames +=
        static_cast<uint64_t>(gAudio.state.device.playback.intermediaryBufferLen);
    uint64_t converterLatency =
        ma_data_converter_get_output_latency(&gAudio.state.device.playback.converter);
    if (converterLatency > 0) {
      latencyFrames +=
          rescaleFrames(converterLatency, internalRate, gAudio.state.sampleRate);
    }
  }
  gAudio.state.deviceDelayFrames = latencyFrames;

  gAudio.nowPlaying.clear();
  return true;
}

void audioStopStream() {
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
  if (writtenFrames) {
    *writtenFrames = 0;
  }
  if (!gAudio.decoderReady || !gAudio.state.externalStream.load()) {
    return false;
  }
  if (!gAudio.state.streamQueueEnabled.load()) {
    return false;
  }
  if (!interleaved || frames == 0) {
    return false;
  }
  if (serial != gAudio.state.streamSerial.load(std::memory_order_relaxed)) {
    return true;
  }
  
  // Discard logic for A/V sync catch-up (established at sync boundary).
  // Any audio data that arrives before the sync point is silently discarded,
  // effectively "fast-forwarding" through old buffered data.
  int64_t discardThreshold = gAudio.state.streamDiscardPtsUs.load(std::memory_order_relaxed);
  if (discardThreshold > 0 && ptsUs < discardThreshold) {
    // This chunk is before the sync point. Discard it entirely.
    // The caller will provide the next chunk with ptsUs >= discardThreshold.
    if (writtenFrames) *writtenFrames = frames;
    return true;
  } else if (discardThreshold > 0 && ptsUs >= discardThreshold) {
    // We've reached the sync point. Clear the discard threshold
    // so subsequent writes proceed normally.
    gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  }

  if (!gAudio.state.streamBaseValid.load(std::memory_order_relaxed)) {
    gAudio.state.streamBasePtsUs.store(ptsUs, std::memory_order_relaxed);
    gAudio.state.streamBaseValid.store(true, std::memory_order_relaxed);
    gAudio.state.streamReadFrames.store(0, std::memory_order_relaxed);
    gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  }

  // Task 1: Store audio metadata in the ringbuffer.
  // Associate the current PTS with the current write position.
  if (ptsUs != AV_NOPTS_VALUE) {
    std::lock_guard<std::mutex> lock(gAudio.state.streamMetadataMutex);
    // Only add if it's a new timestamp or serial jump to keep the queue small
    if (gAudio.state.streamMetadata.empty() || 
        gAudio.state.streamMetadata.back().ptsUs != ptsUs ||
        gAudio.state.streamMetadata.back().serial != serial) {
      gAudio.state.streamMetadata.push_back({
          gAudio.state.streamRb.wpos.load(std::memory_order_relaxed), 
          ptsUs, 
          serial
      });
    }
  }

  uint64_t remaining = frames;
  const float* cursor = interleaved;
  uint64_t totalWritten = 0;
  while (remaining > 0) {
    if (serial != gAudio.state.streamSerial.load(std::memory_order_relaxed)) {
      return true;
    }
    if (!gAudio.state.externalStream.load() ||
        !gAudio.state.streamQueueEnabled.load()) {
      return false;
    }
    uint64_t written = gAudio.state.streamRb.writeSome(cursor, remaining);
    if (written == 0) {
      if (!allowBlock) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    remaining -= written;
    totalWritten += written;
    cursor += static_cast<size_t>(written) * gAudio.channels;
  }
  if (writtenFrames) {
    *writtenFrames = totalWritten;
  }
  return true;
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
  return gAudio.state.audioClock.is_valid();
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
  if (!gAudio.decoderReady || !gAudio.enableAudio) return -1;
  if (gAudio.state.usingKss.load()) return gAudio.kssTrackIndex;
  if (gAudio.state.usingGme.load()) return gAudio.gmeTrackIndex;
  return -1;
}

double audioGetTimeSec() {
  if (!gAudio.decoderReady || !gAudio.enableAudio) {
    return 0.0;
  }
  if (!gAudio.state.audioPrimed.load()) {
    return 0.0;
  }
  int64_t frames = static_cast<int64_t>(gAudio.state.audioClockFrames.load());
  uint64_t latencyFrames = 0;
  if (gAudio.deviceReady &&
      ma_device_get_state(&gAudio.state.device) !=
          ma_device_state_uninitialized) {
    uint32_t internalRate = gAudio.state.device.playback.internalSampleRate;
    uint64_t internalBuffer = 0;
    uint32_t periodFrames = gAudio.state.device.playback.internalPeriodSizeInFrames;
    uint32_t periods = gAudio.state.device.playback.internalPeriods;
    if (periodFrames > 0 && periods > 0) {
      internalBuffer = static_cast<uint64_t>(periodFrames) *
                       static_cast<uint64_t>(periods);
    }
#ifdef MA_SUPPORT_WASAPI
    if (gAudio.state.device.wasapi.actualBufferSizeInFramesPlayback > 0) {
      internalBuffer = std::max<uint64_t>(
          internalBuffer, gAudio.state.device.wasapi.actualBufferSizeInFramesPlayback);
    }
#endif
    if (internalBuffer > 0) {
      latencyFrames +=
          rescaleFrames(internalBuffer, internalRate, gAudio.sampleRate);
    }
    latencyFrames +=
        static_cast<uint64_t>(gAudio.state.device.playback.intermediaryBufferLen);
    uint64_t converterLatency =
        ma_data_converter_get_output_latency(&gAudio.state.device.playback.converter);
    if (converterLatency > 0) {
      latencyFrames +=
          rescaleFrames(converterLatency, internalRate, gAudio.sampleRate);
    }
  }
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
  if (!gAudio.decoderReady || !gAudio.enableAudio) {
    return -1.0;
  }
  uint64_t total = gAudio.state.totalFrames.load();
  if (total == 0) return -1.0;
  return static_cast<double>(total) / gAudio.sampleRate;
}

bool audioIsSeeking() {
  if (!gAudio.decoderReady || !gAudio.enableAudio) {
    return false;
  }
  return gAudio.state.seekRequested.load();
}

double audioGetSeekTargetSec() {
  if (!gAudio.decoderReady || !gAudio.enableAudio) {
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
  if (!gAudio.decoderReady || !gAudio.enableAudio) return false;
  return gAudio.state.audioPrimed.load();
}

bool audioIsPaused() {
  if (!gAudio.decoderReady || !gAudio.enableAudio) return false;
  return gAudio.state.paused.load();
}

bool audioIsFinished() {
  if (!gAudio.decoderReady || !gAudio.enableAudio) return false;
  return gAudio.state.finished.load();
}

bool audioIsRadioEnabled() { return gAudio.state.useRadio1938.load(); }

bool audioIsHolding() {
  if (!gAudio.enableAudio) return false;
  return gAudio.state.hold.load();
}

AudioPerfStats audioGetPerfStats() {
  AudioPerfStats stats{};
  if (!gAudio.decoderReady || !gAudio.enableAudio) {
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
  stats.usingFfmpeg = gAudio.state.usingFfmpeg.load(std::memory_order_relaxed);
  if (gAudio.deviceReady &&
      ma_device_get_state(&gAudio.state.device) !=
          ma_device_state_uninitialized) {
    stats.periodFrames = gAudio.state.device.playback.internalPeriodSizeInFrames;
    stats.periods = gAudio.state.device.playback.internalPeriods;
    stats.bufferFrames = stats.periodFrames * stats.periods;
  }
  return stats;
}

void audioTogglePause() {
  if (!gAudio.decoderReady || !gAudio.enableAudio) return;
  gAudio.state.paused.store(!gAudio.state.paused.load());
}

void audioSeekBy(int direction) {
  if (!gAudio.decoderReady || !gAudio.enableAudio) return;
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
}

void audioSeekToRatio(double ratio) {
  if (!gAudio.decoderReady || !gAudio.enableAudio) return;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total == 0) return;
  ratio = std::clamp(ratio, 0.0, 1.0);
  int64_t target = static_cast<int64_t>(ratio * static_cast<double>(total));
  gAudio.state.pendingSeekFrames.store(target);
  gAudio.state.seekRequested.store(true);
  gAudio.state.finished.store(false);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.m4aCv.notify_all();
}

void audioSeekToSec(double sec) {
  if (!gAudio.decoderReady || !gAudio.enableAudio) return;
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
}

void audioToggleRadio() {
  bool next = !gAudio.state.useRadio1938.load();
  gAudio.state.useRadio1938.store(next);
  if (!gAudio.enableAudio) return;
  uint32_t desired = next ? 1u : gAudio.baseChannels;
  ensureChannels(desired);
}

static void reloadKssWithOptions() {
  if (!gAudio.enableAudio || !gAudio.decoderReady) return;
  if (!gAudio.state.usingKss.load()) return;
  uint64_t resumeFrame = gAudio.state.framesPlayed.load();
  bool wasPaused = gAudio.state.paused.load();
  if (loadFileAt(gAudio.nowPlaying, resumeFrame, gAudio.kssTrackIndex)) {
    if (wasPaused) {
      gAudio.state.paused.store(true);
    }
  }
}

void audioToggle50Hz() {
  gAudio.kssOptions.force50Hz = !gAudio.kssOptions.force50Hz;
  reloadKssWithOptions();
}

void audioSetHold(bool hold) {
  if (!gAudio.enableAudio) return;
  gAudio.state.hold.store(hold);
}

void audioAdjustVolume(float delta) {
  float current = gAudio.state.volume.load(std::memory_order_relaxed);
  float next = std::clamp(current + delta, 0.0f, 4.0f);
  gAudio.state.volume.store(next, std::memory_order_relaxed);
}

float audioGetVolume() {
  return gAudio.state.volume.load(std::memory_order_relaxed);
}

std::string audioGetWarning() {
  return gAudio.gmeWarning;
}

bool audioIs50HzEnabled() {
  return gAudio.kssOptions.force50Hz;
}

KssPlaybackOptions audioGetKssOptionState() {
  return gAudio.kssOptions;
}

bool audioAdjustKssOption(KssOptionId id, int direction) {
  if (direction == 0) return false;
  bool changed = false;
  switch (id) {
    case KssOptionId::Force50Hz:
      gAudio.kssOptions.force50Hz = !gAudio.kssOptions.force50Hz;
      changed = true;
      break;
    case KssOptionId::SccType: {
      int next = static_cast<int>(gAudio.kssOptions.sccType) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      gAudio.kssOptions.sccType = static_cast<KssSccType>(next);
      changed = true;
      break;
    }
    case KssOptionId::PsgQuality: {
      int next = static_cast<int>(gAudio.kssOptions.psgQuality) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      gAudio.kssOptions.psgQuality = static_cast<KssQuality>(next);
      changed = true;
      break;
    }
    case KssOptionId::SccQuality: {
      int next = static_cast<int>(gAudio.kssOptions.sccQuality) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      gAudio.kssOptions.sccQuality = static_cast<KssQuality>(next);
      changed = true;
      break;
    }
    case KssOptionId::OpllStereo:
      gAudio.kssOptions.opllStereo = !gAudio.kssOptions.opllStereo;
      changed = true;
      break;
    case KssOptionId::MutePsg:
      gAudio.kssOptions.mutePsg = !gAudio.kssOptions.mutePsg;
      changed = true;
      break;
    case KssOptionId::MuteScc:
      gAudio.kssOptions.muteScc = !gAudio.kssOptions.muteScc;
      changed = true;
      break;
    case KssOptionId::MuteOpll:
      gAudio.kssOptions.muteOpll = !gAudio.kssOptions.muteOpll;
      changed = true;
      break;
    default:
      break;
  }
  if (changed) {
    reloadKssWithOptions();
  }
  return changed;
}
