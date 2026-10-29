#include "audio/audioplayback.h"
#include "audio/audioplayback_internal.h"
#include "audio/playback_device.h"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>

namespace {
void expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void silenceCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
  auto& state = *static_cast<AudioState*>(device->pUserData);
  state.callbackCount.fetch_add(1);
  ma_silence_pcm_frames(output, frames, ma_format_f32, 1);
}

void waitForCallback(AudioState& state, uint64_t previous) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (state.callbackCount.load() <= previous) {
    expect(std::chrono::steady_clock::now() < deadline,
           "Recovered output must deliver callbacks without a Play command");
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
}
}  // namespace

int main() try {
  // Use miniaudio's real worker and state transitions with its null backend.
  // These tests neither require nor open a physical audio endpoint.
  ma_context context{};
  const ma_backend backend = ma_backend_null;
  expect(ma_context_init(&backend, 1, nullptr, &context) == MA_SUCCESS,
         "Null audio context must initialize");
  auto owned = std::make_unique<AudioPlaybackState>();
  auto& audio = *owned;
  auto& state = audio.state;
  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = 1;
  config.sampleRate = 48000;
  config.dataCallback = silenceCallback;
  config.pUserData = &state;
  expect(ma_device_init(&context, &config, &state.device) == MA_SUCCESS,
         "Null output must initialize");
  audio.deviceReady = true;
  expect(ma_device_start(&state.device) == MA_SUCCESS, "Null output must start");
  waitForCallback(state, 0);

  for (bool paused : {false, true}) {
    state.paused.store(paused);
    expect(ma_device_stop(&state.device) == MA_SUCCESS,
           "Simulated endpoint interruption must stop the worker");
    state.deviceRecoveryPending.store(true);
    AudioPerfStats stats;
    audioPlaybackDeviceFillPerfStats(audio, &stats);
    expect(stats.outputRecovering, "Stopped output must be visible to video sync");
    const auto callbacks = state.callbackCount.load();
    audioPlaybackDeviceService(audio);
    expect(state.device.pContext == &context,
           "A stopped rerouted device must be restarted before recreating it");
    waitForCallback(state, callbacks);
    expect(state.paused.load() == paused,
           "Device recovery must preserve the user's transport pause state");
    expect(!state.deviceRecoveryPending.load(), "Recovery request must be consumed");
    audioPlaybackDeviceFillPerfStats(audio, &stats);
    expect(!stats.outputRecovering, "Resumed output must release video sync");
  }

  // A reroute that already resumed must not be restarted a second time.
  state.deviceRecoveryPending.store(true);
  state.streamClockReady.store(true);
  audioPlaybackDeviceService(audio);
  expect(state.streamClockReady.load(), "A healthy reroute must keep its audio clock");
  expect(state.device.pContext == &context, "Healthy output must retain its context");

  audioPlaybackDeviceUninit(audio);
  // Also cancel a pending retry when initialization had already failed.
  state.deviceRecoveryPending.store(true);
  audioPlaybackDeviceUninit(audio);
  audioPlaybackDeviceService(audio);
  expect(!audio.deviceReady, "Polling after shutdown must not reopen the output");
  ma_context_uninit(&context);
  std::cout << "audio_device_recovery_tests: interruption, paused recovery, "
               "healthy reroute and shutdown passed\n";
  return 0;
} catch (const std::exception& error) {
  std::cerr << "audio_device_recovery_tests: " << error.what() << '\n';
  return 1;
}
