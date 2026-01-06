#include "audioplayback.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "ffmpegaudio.h"
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

bool isSupportedAudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac" || ext == ".m4a" ||
         ext == ".webm" || ext == ".mp4";
}

bool isMiniaudioExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

bool isM4aExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".m4a" || ext == ".mp4" || ext == ".webm";
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

struct AudioState {
  ma_decoder decoder{};
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
  std::atomic<bool> paused{false};
  std::atomic<bool> hold{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> useRadio1938{true};
  std::atomic<bool> usingFfmpeg{false};
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

  if (state->hold.load()) {
    state->silentFrames.fetch_add(frameCount, std::memory_order_relaxed);
    state->lastFramesRead.store(0, std::memory_order_relaxed);
    std::fill(out, out + frameCount * state->channels, 0.0f);
    return;
  }

  bool usingFfmpeg = state->usingFfmpeg.load();
  if (!usingFfmpeg && state->seekRequested.exchange(false)) {
    int64_t target = state->pendingSeekFrames.load();
    if (target < 0) target = 0;
    ma_decoder_seek_to_pcm_frame(&state->decoder,
                                 static_cast<ma_uint64>(target));
    state->framesPlayed.store(static_cast<uint64_t>(target));
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
      if (state->seekRequested.load()) {
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

bool loadFileAt(const std::filesystem::path& file, uint64_t startFrame) {
  if (!gAudio.enableAudio) {
    return false;
  }
  validateInputFile(file);
  const bool useM4a = isM4aExt(file);
  const bool useMiniaudio = isMiniaudioExt(file);
  if (!useM4a && !useMiniaudio) {
    return false;
  }
  if (gAudio.deviceReady) {
    ma_device_stop(&gAudio.state.device);
  }
  if (gAudio.decoderReady) {
    if (gAudio.state.usingFfmpeg.load()) {
      stopM4aWorker();
    } else {
      ma_decoder_uninit(&gAudio.state.decoder);
    }
    gAudio.decoderReady = false;
    gAudio.state.usingFfmpeg.store(false);
  }

  if (useM4a) {
    std::string error;
    gAudio.state.totalFrames.store(0);
    if (!startM4aWorker(file, startFrame, &error)) {
      return false;
    }
    gAudio.decoderReady = true;
    gAudio.state.usingFfmpeg.store(true);
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

    ma_uint64 totalFrames = 0;
    if (ma_decoder_get_length_in_pcm_frames(&gAudio.state.decoder,
                                            &totalFrames) == MA_SUCCESS) {
      gAudio.state.totalFrames.store(totalFrames);
    } else {
      gAudio.state.totalFrames.store(0);
    }
  }

  if (!useM4a) {
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

bool loadFile(const std::filesystem::path& file) {
  return loadFileAt(file, 0);
}

bool ensureChannels(uint32_t newChannels) {
  if (newChannels == gAudio.channels) return true;

  uint64_t resumeFrame = gAudio.decoderReady ? gAudio.state.framesPlayed.load()
                                             : 0;
  bool hadTrack = gAudio.decoderReady && !gAudio.nowPlaying.empty();

  if (gAudio.deviceReady) {
    ma_device_stop(&gAudio.state.device);
    ma_device_uninit(&gAudio.state.device);
    gAudio.deviceReady = false;
  }
  if (gAudio.decoderReady) {
    if (gAudio.state.usingFfmpeg.load()) {
      stopM4aWorker();
    } else {
      ma_decoder_uninit(&gAudio.state.decoder);
    }
    gAudio.decoderReady = false;
    gAudio.state.usingFfmpeg.store(false);
  }

  gAudio.channels = newChannels;
  gAudio.state.channels = gAudio.channels;
  gAudio.radio1938Template.init(gAudio.channels,
                                static_cast<float>(gAudio.sampleRate),
                                gAudio.lpHz,
                                gAudio.noise);
  gAudio.state.radio1938 = gAudio.radio1938Template;

  if (hadTrack) {
    return loadFileAt(gAudio.nowPlaying, resumeFrame);
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
    } else {
      ma_decoder_uninit(&gAudio.state.decoder);
    }
    gAudio.decoderReady = false;
    gAudio.state.usingFfmpeg.store(false);
  }
  gAudio.state.framesPlayed.store(0);
  gAudio.state.audioClockFrames.store(0);
  gAudio.state.finished.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingSeekFrames.store(0);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.paused.store(false);
  gAudio.state.hold.store(false);
  gAudio.nowPlaying.clear();
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

bool audioStartFile(const std::filesystem::path& file) {
  if (!gAudio.enableAudio) return false;
  return loadFile(file);
}

bool audioStartFileAt(const std::filesystem::path& file, double startSec) {
  if (!gAudio.enableAudio) return false;
  if (!std::isfinite(startSec) || startSec <= 0.0) {
    return loadFile(file);
  }
  double framesDouble = startSec * static_cast<double>(gAudio.sampleRate);
  int64_t startFrame = static_cast<int64_t>(std::llround(framesDouble));
  if (startFrame < 0) startFrame = 0;
  return loadFileAt(file, static_cast<uint64_t>(startFrame));
}

void audioStop() { stopPlayback(); }

std::filesystem::path audioGetNowPlaying() { return gAudio.nowPlaying; }

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

void audioSetHold(bool hold) {
  if (!gAudio.enableAudio) return;
  gAudio.state.hold.store(hold);
}
