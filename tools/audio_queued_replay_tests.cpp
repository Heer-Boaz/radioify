#include "audio/audioplayback_internal.h"
#include "audio/playback_backend.h"
#include "audio/queued_audio_source.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

void expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

struct SeekGate {
  std::mutex mutex;
  std::condition_variable changed;
  std::atomic<bool> entered{false};
  bool armed = false;
  bool released = false;
  BackendSeekProc seek = nullptr;
};

SeekGate seekGate;

bool seekWithGate(AudioPlaybackState& audio, uint64_t frame) {
  {
    std::unique_lock<std::mutex> lock(seekGate.mutex);
    if (seekGate.armed) {
      seekGate.armed = false;
      seekGate.entered.store(true, std::memory_order_release);
      seekGate.changed.notify_all();
      seekGate.changed.wait(lock, [] { return seekGate.released; });
    }
  }
  return seekGate.seek(audio, frame);
}

void releaseSeekGate() {
  {
    std::lock_guard<std::mutex> lock(seekGate.mutex);
    seekGate.released = true;
  }
  seekGate.changed.notify_all();
}

void put16(std::vector<uint8_t>& bytes, uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void put32(std::vector<uint8_t>& bytes, uint32_t value) {
  put16(bytes, static_cast<uint16_t>(value));
  put16(bytes, static_cast<uint16_t>(value >> 16));
}

std::vector<uint8_t> makeWave(uint32_t frames) {
  std::vector<uint8_t> bytes;
  const auto fourcc = [&](const char* text) {
    bytes.insert(bytes.end(), text, text + 4);
  };
  fourcc("RIFF");
  put32(bytes, 36 + frames * 2);
  fourcc("WAVE");
  fourcc("fmt ");
  put32(bytes, 16);
  put16(bytes, 1);       // PCM
  put16(bytes, 1);       // mono
  put32(bytes, 48000);
  put32(bytes, 96000);
  put16(bytes, 2);
  put16(bytes, 16);
  fourcc("data");
  put32(bytes, frames * 2);
  for (uint32_t i = 0; i < frames; ++i) {
    const int16_t sample = i < frames / 2 ? 16000 : -16000;
    put16(bytes, static_cast<uint16_t>(sample));
  }
  return bytes;
}

// Exercise the real decoder, worker, DSP queue, transitions and callback
// without depending on a physical playback device or a wall-clock audio rate.
class PlaybackFixture {
 public:
  explicit PlaybackFixture(uint32_t frames, bool blockFirstSeek = false)
      : wave_(makeWave(frames)), audio_(std::make_unique<AudioPlaybackState>()) {
    auto& state = audio_->state;
    state.dry = true;
    RadioPlaybackFilterConfig filter;
    filter.initialMode = RadioFilterMode::Off;
    state.radioFilter.initialize(filter);
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 1, 48000);
    expect(ma_decoder_init_memory(wave_.data(), wave_.size(), &config,
                                  &state.decoder) == MA_SUCCESS,
           "WAV decoder must initialize");
    decoderReady_ = true;
    state.totalFrames.store(frames);
    const AudioBackendHandlers* backend = selectAudioBackend("test.wav");
    expect(backend && backend->mode == AudioMode::Miniaudio,
           "WAV must use the queued miniaudio backend");
    if (blockFirstSeek) {
      backend_ = *backend;
      {
        std::lock_guard<std::mutex> lock(seekGate.mutex);
        seekGate.seek = backend->seek;
        seekGate.armed = true;
        seekGate.released = false;
        seekGate.entered.store(false, std::memory_order_relaxed);
      }
      backend_.seek = seekWithGate;
      backend = &backend_;
    }
    queuedAudioSourceStartProcessing(&state, 8192, 8192, 1000, 0, 0);
    expect(queuedAudioSourceStartDecoderWorker(*audio_, backend, 0),
           "Decoder worker must start");
    device_.pUserData = &state;
  }

  ~PlaybackFixture() {
    releaseSeekGate();
    auto& state = audio_->state;
    queuedAudioSourceStopDecoderWorker(&state);
    queuedAudioSourceStopProcessing(&state);
    if (decoderReady_) ma_decoder_uninit(&state.decoder);
  }

  AudioPlaybackState& audio() { return *audio_; }

  template <typename Predicate>
  void wait(Predicate predicate, const char* message, bool pump = false) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        const auto& state = audio_->state;
        std::cerr << "phase=" << static_cast<int>(state.pipelineTransition.phase.load())
                  << " request=" << state.pipelineTransition.requestSerial.load()
                  << " commit=" << state.pipelineTransition.commitSerial.load()
                  << " seeking=" << state.seekRequested.load()
                  << " source_reset=" << state.sourceResetRequestGeneration.load()
                  << "/" << state.sourceResetAppliedGeneration.load()
                  << " decoded=" << state.decodedAudio.bufferedFrames()
                  << " processed=" << state.processedAudio.bufferedFrames()
                  << " source_end=" << state.sourceAtEnd.load()
                  << " processed_end=" << state.processedAtEnd.load()
                  << " decoder_running=" << state.decodeThreadRunning.load()
                  << " dsp_running=" << state.radioDspThreadRunning.load()
                  << '\n';
        throw std::runtime_error(message);
      }
      if (pump) callback();
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }

  void seek(uint64_t target) {
    audioPlaybackRequestSeekFrame(*audio_, static_cast<int64_t>(target));
    wait([&] { return !audio_->state.seekRequested.load(); },
         "Seek must finish while the decoder is at EOF", true);
  }

  void expectAudioSign(bool positive) {
    bool found = false;
    output_.clear();
    wait([&] {
      for (float sample : output_) {
        if (positive ? sample > .1f : sample < -.1f) found = true;
      }
      return found;
    }, "Expected audio from the requested position", true);
  }

 private:
  void callback() {
    output_.assign(128, 0.0f);
    dataCallback(&device_, output_.data(), nullptr,
                 static_cast<ma_uint32>(output_.size()));
  }

  std::vector<uint8_t> wave_;
  std::unique_ptr<AudioPlaybackState> audio_;
  ma_device device_{};
  std::vector<float> output_;
  AudioBackendHandlers backend_{};
  bool decoderReady_ = false;
};

class StreamFixture {
 public:
  StreamFixture() : audio_(std::make_unique<AudioPlaybackState>()) {
    auto& state = audio_->state;
    state.dry = true;
    state.externalStream.store(true);
    RadioPlaybackFilterConfig filter;
    filter.initialMode = RadioFilterMode::Off;
    state.radioFilter.initialize(filter);
    queuedAudioSourceStartProcessing(&state, 8192, 8192, 1000, 0, 0);
    device_.pUserData = &state;
  }

  ~StreamFixture() { queuedAudioSourceStopProcessing(&audio_->state); }

  AudioState& state() { return audio_->state; }

  template <typename Predicate>
  bool pumpUntil(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(3);
    std::vector<float> output(128);
    while (!predicate()) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      dataCallback(&device_, output.data(), nullptr,
                   static_cast<ma_uint32>(output.size()));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }

 private:
  std::unique_ptr<AudioPlaybackState> audio_;
  ma_device device_{};
};

void seekAndReplay() {
  constexpr uint32_t frames = 4800;
  PlaybackFixture fixture(frames);
  auto& state = fixture.audio().state;
  fixture.wait([&] { return state.processedAtEnd.load(); },
               "Short source must finish decoding and processing");
  expect(state.decodeThreadRunning.load(),
         "Decoder must stay available after decoding EOF");
  expect(state.processedAudio.bufferedFrames() == frames,
         "EOF must not discard buffered audio");

  // Seek after the decoder has finished, before the output queue is drained.
  fixture.seek(frames / 2);
  fixture.expectAudioSign(false);
  fixture.wait([&] { return state.finished.load(); },
               "Sought audio must reach output EOF", true);

  // Replaying a drained track must seek and actually emit its opening samples.
  for (int replay = 0; replay < 2; ++replay) {
    expect(state.decodeThreadRunning.load(), "EOF must not stop the decoder");
    fixture.seek(0);
    fixture.expectAudioSign(true);
    fixture.wait([&] { return state.finished.load(); },
                 "Replayed audio must reach EOF", true);
  }

  // The worker must also exit if processing shuts down before it is joined.
  queuedAudioSourceStopProcessing(&state);
  fixture.wait([&] { return !state.decodeThreadRunning.load(); },
               "A parked decoder must stop when its queue is disabled");
}

void replayShorterThanPrimeBuffer() {
  PlaybackFixture fixture(240);
  auto& state = fixture.audio().state;
  fixture.wait([&] { return state.processedAtEnd.load(); },
               "Short source must publish EOF before priming");
  fixture.wait([&] { return state.finished.load(); },
               "Short source must reach output EOF", true);
  fixture.seek(0);
  fixture.wait([&] { return state.finished.load(); },
               "Replayed short source must finish its seek and drain", true);
  expect(state.decodeThreadRunning.load(),
         "A short replay must leave the decoder parked at EOF");
}

void supersededSeekUsesLatestTarget() {
  constexpr uint32_t frames = 4800;
  PlaybackFixture fixture(frames, true);
  auto& state = fixture.audio().state;
  fixture.wait([&] { return state.processedAtEnd.load(); },
               "Source must finish decoding before the interrupted seek");
  audioPlaybackRequestSeekFrame(fixture.audio(), 0);
  fixture.wait([&] { return seekGate.entered.load(std::memory_order_acquire); },
               "The first seek must reach the decoder", true);
  // The first decoder seek is still in progress when the second arrives.
  audioPlaybackRequestSeekFrame(fixture.audio(), frames / 2);
  releaseSeekGate();
  fixture.wait([&] { return !state.seekRequested.load(); },
               "A superseding seek must finish", true);
  fixture.expectAudioSign(false);
  expect(state.audioClockFrames.load() >= frames / 2,
         "Playback must continue from the latest seek target");
}

void repeatedSeeksResumeFromTheRequestedPosition() {
  constexpr uint32_t frames = 4800;
  PlaybackFixture fixture(frames);
  auto& state = fixture.audio().state;
  fixture.wait([&] { return state.processedAtEnd.load(); },
               "Source must finish decoding before repeated seeks");
  for (int i = 0; i < 100; ++i) {
    const bool seekToStart = i % 2 == 0;
    fixture.seek(seekToStart ? 0 : frames / 2);
    fixture.expectAudioSign(seekToStart);
  }
}

void streamResetsKeepAudioClockInTheNewTimeline() {
  StreamFixture fixture;
  auto& state = fixture.state();
  const std::vector<float> samples(1500, .5f);
  for (int serial = 1; serial <= 10; ++serial) {
    const uint64_t frame = static_cast<uint64_t>(serial) * 48000;
    const int64_t ptsUs = static_cast<int64_t>(serial) * 1000000;
    const uint64_t generation = queuedAudioSourceRequestStreamReset(
        &state, serial, ptsUs, frame, true);
    std::atomic<bool> done{false};
    bool applied = false;
    std::thread waiter([&] {
      applied = queuedAudioSourceWaitForStreamReset(&state, generation);
      done.store(true, std::memory_order_release);
    });
    const bool completed = fixture.pumpUntil(
        [&] { return done.load(std::memory_order_acquire); });
    if (!completed) queuedAudioSourceStopProcessing(&state);
    waiter.join();
    expect(completed && applied, "Stream reset must wake its waiting owner");
    const AudioStreamReset reset = queuedAudioSourceLastAppliedStreamReset(&state);
    expect(reset.generation == generation && reset.serial == serial &&
               reset.framePosition == frame && reset.discardUntilUs == ptsUs,
           "Stream reset must apply the requested timeline");

    const uint64_t previousFramesRead = state.framesReadTotal.load();
    uint64_t written = 0;
    expect(queuedAudioSourceWrite(&state, samples.data(), samples.size(), ptsUs,
                                  serial, false, false, &written) &&
               written == samples.size(),
           "New stream audio must enter the reset timeline");
    expect(fixture.pumpUntil([&] {
             return !audioPipelineTransitionActive(state.pipelineTransition) &&
                    state.framesReadTotal.load() > previousFramesRead;
           }), "New stream audio must resume after the reset");
    expect(state.audioClock.serial.load() == serial &&
               state.audioClockFrames.load() >= frame,
           "Output audio clock must follow the new stream serial and position");
  }
}

void decoderWakeRetainsAnEarlySignal() {
  AudioDecoderWake wake;
  wake.signal();
  wake.wait();
}

void stopWithSeekAwaitingOutputCallback() {
  PlaybackFixture fixture(4800);
  auto& state = fixture.audio().state;
  fixture.wait([&] { return state.processedAtEnd.load(); },
               "Source must finish decoding before cancellation");
  audioPlaybackRequestSeekFrame(fixture.audio(), 0);
  // Do not pump the callback: shutdown must wake a decoder waiting for it.
  queuedAudioSourceStopDecoderWorker(&state);
  expect(!state.decodeThreadRunning.load(),
         "Stopping must release a decoder waiting for the output callback");
}

}  // namespace

int main() try {
  seekAndReplay();
  replayShorterThanPrimeBuffer();
  supersededSeekUsesLatestTarget();
  repeatedSeeksResumeFromTheRequestedPosition();
  streamResetsKeepAudioClockInTheNewTimeline();
  decoderWakeRetainsAnEarlySignal();
  stopWithSeekAwaitingOutputCallback();
  std::cout << "audio_queued_replay_tests: buffered seek, repeated replay, "
               "superseded seeks, stream resets and shutdown passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "audio_queued_replay_tests: " << error.what() << '\n';
  return 1;
}
