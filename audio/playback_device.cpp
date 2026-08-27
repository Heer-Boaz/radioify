#include "playback_device.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include "audioplayback.h"
#include "audioplayback_internal.h"

namespace {

static uint64_t rescalePlaybackDeviceFrames(uint64_t frames,
                                            uint32_t inRate,
                                            uint32_t outRate) {
  if (frames == 0 || inRate == 0 || outRate == 0 || inRate == outRate) {
    return frames;
  }
  double scaled = static_cast<double>(frames) * static_cast<double>(outRate) /
                  static_cast<double>(inRate);
  return static_cast<uint64_t>(std::llround(scaled));
}

static void requestPlaybackDeviceDeclickRampUnlocked(
    AudioPlaybackState& audio) {
  audioPipelineTransitionRequestOutputFadeIn(audio.state.pipelineTransition,
                                             audio.state.sampleRate);
}

static void invalidatePlaybackDeviceRuntimeState(AudioPlaybackState& audio) {
  audio.state.audioPrimed.store(false, std::memory_order_relaxed);
  audio.state.streamClockReady.store(false, std::memory_order_relaxed);
  audio.state.streamStarved.store(true, std::memory_order_relaxed);
  audio.state.audioClock.invalidate();
  audio.state.deviceDelayFrames.store(0, std::memory_order_relaxed);
  requestPlaybackDeviceDeclickRampUnlocked(audio);
}

static uint64_t calculatePlaybackDeviceLatencyFramesUnlocked(
    AudioPlaybackState& audio) {
  if (!audio.deviceReady ||
      ma_device_get_state(&audio.state.device) ==
          ma_device_state_uninitialized) {
    return 0;
  }

  uint64_t latencyFrames = 0;
  uint32_t internalRate = audio.state.device.playback.internalSampleRate;
  uint64_t internalBuffer = 0;
  uint32_t periodFrames =
      audio.state.device.playback.internalPeriodSizeInFrames;
  uint32_t periods = audio.state.device.playback.internalPeriods;
  if (periodFrames > 0 && periods > 0) {
    internalBuffer = static_cast<uint64_t>(periodFrames) *
                     static_cast<uint64_t>(periods);
  }
#ifdef MA_SUPPORT_WASAPI
  if (audio.state.device.wasapi.actualBufferSizeInFramesPlayback > 0) {
    internalBuffer = std::max<uint64_t>(
        internalBuffer, audio.state.device.wasapi.actualBufferSizeInFramesPlayback);
  }
#endif
  if (internalBuffer > 0) {
    latencyFrames += rescalePlaybackDeviceFrames(
        internalBuffer, internalRate, audio.state.sampleRate);
  }
  latencyFrames +=
      static_cast<uint64_t>(audio.state.device.playback.intermediaryBufferLen);
  uint64_t converterLatency = ma_data_converter_get_output_latency(
      &audio.state.device.playback.converter);
  if (converterLatency > 0) {
    latencyFrames += rescalePlaybackDeviceFrames(
        converterLatency, internalRate, audio.state.sampleRate);
  }

  return latencyFrames;
}

static void updatePlaybackDeviceDelayFramesUnlocked(
    AudioPlaybackState& audio) {
  audio.state.deviceDelayFrames.store(
      calculatePlaybackDeviceLatencyFramesUnlocked(audio),
      std::memory_order_relaxed);
}

static void deviceNotificationCallback(
    const ma_device_notification* notification) {
  if (!notification || !notification->pDevice ||
      !notification->pDevice->pUserData) {
    return;
  }
  auto& state =
      *static_cast<AudioState*>(notification->pDevice->pUserData);
  if (notification->pDevice != &state.device) return;

  switch (notification->type) {
    case ma_device_notification_type_started:
      state.deviceStopExpected.store(false, std::memory_order_relaxed);
      break;
    case ma_device_notification_type_stopped:
      if (!state.deviceStopExpected.load(std::memory_order_relaxed)) {
        state.deviceRecoveryPending.store(true, std::memory_order_relaxed);
      }
      break;
    case ma_device_notification_type_rerouted:
    case ma_device_notification_type_interruption_began:
    case ma_device_notification_type_interruption_ended:
      state.deviceRecoveryPending.store(true, std::memory_order_relaxed);
      break;
    case ma_device_notification_type_unlocked:
      break;
  }
}

static bool initPlaybackDeviceUnlocked(AudioPlaybackState& audio) {
  if (audio.deviceReady) return true;

  ma_device_config devConfig = ma_device_config_init(ma_device_type_playback);
  devConfig.playback.format = ma_format_f32;
  devConfig.playback.channels = audio.channels;
  devConfig.sampleRate = audio.sampleRate;
  devConfig.dataCallback = dataCallback;
  devConfig.notificationCallback = deviceNotificationCallback;
  devConfig.pUserData = &audio.state;

  if (ma_device_init(nullptr, &devConfig, &audio.state.device) != MA_SUCCESS) {
    audio.lastInitError = "Failed to initialize audio output device.";
    invalidatePlaybackDeviceRuntimeState(audio);
    return false;
  }
  requestPlaybackDeviceDeclickRampUnlocked(audio);
  if (ma_device_start(&audio.state.device) != MA_SUCCESS) {
    ma_device_uninit(&audio.state.device);
    audio.lastInitError = "Failed to start audio output device.";
    invalidatePlaybackDeviceRuntimeState(audio);
    return false;
  }

  audio.deviceReady = true;
  audio.state.deviceRecoveryPending.store(false, std::memory_order_relaxed);
  audio.state.deviceStopExpected.store(false, std::memory_order_relaxed);
  updatePlaybackDeviceDelayFramesUnlocked(audio);
#if RADIOIFY_ENABLE_TIMING_LOG
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "audio_device_started sampleRate=%u channels=%u",
                  audio.sampleRate, audio.channels);
    appendAudioTimingLogLine(buf);
  }
#endif
  return true;
}

static void uninitPlaybackDeviceUnlocked(AudioPlaybackState& audio) {
  if (!audio.deviceReady) return;
  if (ma_device_get_state(&audio.state.device) ==
      ma_device_state_uninitialized) {
    audio.deviceReady = false;
    invalidatePlaybackDeviceRuntimeState(audio);
    audio.state.deviceRecoveryPending.store(false, std::memory_order_relaxed);
    audio.state.deviceStopExpected.store(false, std::memory_order_relaxed);
    return;
  }

  audio.state.deviceStopExpected.store(true, std::memory_order_relaxed);
  // Miniaudio explicitly allows uninit without a prior stop when the backend
  // may have drifted out of sync after OS-level device changes.
  ma_device_uninit(&audio.state.device);
  audio.deviceReady = false;
  invalidatePlaybackDeviceRuntimeState(audio);
  audio.state.deviceRecoveryPending.store(false, std::memory_order_relaxed);
  audio.state.deviceStopExpected.store(false, std::memory_order_relaxed);
}

static bool ensurePlaybackDeviceRunningInternalUnlocked(
    AudioPlaybackState& audio) {
  if (!audio.deviceReady) {
    return initPlaybackDeviceUnlocked(audio);
  }

  if (ma_device_get_state(&audio.state.device) ==
          ma_device_state_uninitialized ||
      audio.state.deviceRecoveryPending.load(std::memory_order_relaxed)) {
    uninitPlaybackDeviceUnlocked(audio);
    return initPlaybackDeviceUnlocked(audio);
  }

  const ma_device_state deviceState = ma_device_get_state(&audio.state.device);
  if (deviceState == ma_device_state_started ||
      deviceState == ma_device_state_starting) {
    updatePlaybackDeviceDelayFramesUnlocked(audio);
    return true;
  }

  requestPlaybackDeviceDeclickRampUnlocked(audio);
  if (ma_device_start(&audio.state.device) != MA_SUCCESS) {
    uninitPlaybackDeviceUnlocked(audio);
    return initPlaybackDeviceUnlocked(audio);
  }

  audio.state.deviceStopExpected.store(false, std::memory_order_relaxed);
  audio.state.deviceRecoveryPending.store(false, std::memory_order_relaxed);
  updatePlaybackDeviceDelayFramesUnlocked(audio);
  return true;
}

}  // namespace

bool audioPlaybackDeviceEnsureRunning(AudioPlaybackState& audio) {
  std::lock_guard<std::mutex> lock(audio.playbackDeviceMutex);
  return ensurePlaybackDeviceRunningInternalUnlocked(audio);
}

void audioPlaybackDeviceUninit(AudioPlaybackState& audio) {
  std::lock_guard<std::mutex> lock(audio.playbackDeviceMutex);
  uninitPlaybackDeviceUnlocked(audio);
}

uint64_t audioPlaybackDeviceLatencyFrames(AudioPlaybackState& audio) {
  std::lock_guard<std::mutex> lock(audio.playbackDeviceMutex);
  updatePlaybackDeviceDelayFramesUnlocked(audio);
  return audio.state.deviceDelayFrames.load(std::memory_order_relaxed);
}

void audioPlaybackDeviceFillPerfStats(AudioPlaybackState& audio,
                                      AudioPerfStats* stats) {
  if (!stats) return;
  std::lock_guard<std::mutex> lock(audio.playbackDeviceMutex);
  if (!audio.deviceReady ||
      ma_device_get_state(&audio.state.device) ==
          ma_device_state_uninitialized) {
    return;
  }

  stats->periodFrames = audio.state.device.playback.internalPeriodSizeInFrames;
  stats->periods = audio.state.device.playback.internalPeriods;
  stats->bufferFrames = stats->periodFrames * stats->periods;
}
