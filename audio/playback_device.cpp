#include "playback_device.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>

#include "audioplayback.h"
#include "audioplayback_internal.h"

namespace {

std::mutex gPlaybackDeviceMutex;

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

static void invalidatePlaybackDeviceRuntimeState() {
  gAudio.state.audioPrimed.store(false, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(true, std::memory_order_relaxed);
  gAudio.state.audioClock.invalidate();
  gAudio.state.deviceDelayFrames.store(0, std::memory_order_relaxed);
}

static uint64_t calculatePlaybackDeviceLatencyFramesUnlocked() {
  if (!gAudio.deviceReady ||
      ma_device_get_state(&gAudio.state.device) ==
          ma_device_state_uninitialized) {
    return 0;
  }

  uint64_t latencyFrames = 0;
  uint32_t internalRate = gAudio.state.device.playback.internalSampleRate;
  uint64_t internalBuffer = 0;
  uint32_t periodFrames =
      gAudio.state.device.playback.internalPeriodSizeInFrames;
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
    latencyFrames += rescalePlaybackDeviceFrames(
        internalBuffer, internalRate, gAudio.state.sampleRate);
  }
  latencyFrames +=
      static_cast<uint64_t>(gAudio.state.device.playback.intermediaryBufferLen);
  uint64_t converterLatency = ma_data_converter_get_output_latency(
      &gAudio.state.device.playback.converter);
  if (converterLatency > 0) {
    latencyFrames += rescalePlaybackDeviceFrames(
        converterLatency, internalRate, gAudio.state.sampleRate);
  }

  return latencyFrames;
}

static void updatePlaybackDeviceDelayFramesUnlocked() {
  gAudio.state.deviceDelayFrames.store(
      calculatePlaybackDeviceLatencyFramesUnlocked(), std::memory_order_relaxed);
}

static void deviceNotificationCallback(
    const ma_device_notification* notification) {
  if (!notification || notification->pDevice != &gAudio.state.device) {
    return;
  }

  switch (notification->type) {
    case ma_device_notification_type_started:
      gAudio.state.deviceStopExpected.store(false, std::memory_order_relaxed);
      break;
    case ma_device_notification_type_stopped:
      if (!gAudio.state.deviceStopExpected.load(std::memory_order_relaxed)) {
        gAudio.state.deviceRecoveryPending.store(true,
                                                std::memory_order_relaxed);
      }
      break;
    case ma_device_notification_type_rerouted:
    case ma_device_notification_type_interruption_began:
    case ma_device_notification_type_interruption_ended:
      gAudio.state.deviceRecoveryPending.store(true,
                                              std::memory_order_relaxed);
      break;
    case ma_device_notification_type_unlocked:
      break;
  }
}

static bool initPlaybackDeviceUnlocked() {
  if (gAudio.deviceReady) return true;

  ma_device_config devConfig = ma_device_config_init(ma_device_type_playback);
  devConfig.playback.format = ma_format_f32;
  devConfig.playback.channels = gAudio.channels;
  devConfig.sampleRate = gAudio.sampleRate;
  devConfig.dataCallback = dataCallback;
  devConfig.notificationCallback = deviceNotificationCallback;
  devConfig.pUserData = &gAudio.state;

  if (ma_device_init(nullptr, &devConfig, &gAudio.state.device) != MA_SUCCESS) {
    gAudio.lastInitError = "Failed to initialize audio output device.";
    invalidatePlaybackDeviceRuntimeState();
    return false;
  }
  if (ma_device_start(&gAudio.state.device) != MA_SUCCESS) {
    ma_device_uninit(&gAudio.state.device);
    gAudio.lastInitError = "Failed to start audio output device.";
    invalidatePlaybackDeviceRuntimeState();
    return false;
  }

  gAudio.deviceReady = true;
  gAudio.state.deviceRecoveryPending.store(false, std::memory_order_relaxed);
  gAudio.state.deviceStopExpected.store(false, std::memory_order_relaxed);
  updatePlaybackDeviceDelayFramesUnlocked();
#if RADIOIFY_ENABLE_TIMING_LOG
  {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "audio_device_started sampleRate=%u channels=%u",
                  gAudio.sampleRate, gAudio.channels);
    appendAudioTimingLogLine(buf);
  }
#endif
  return true;
}

static void uninitPlaybackDeviceUnlocked() {
  if (!gAudio.deviceReady) return;
  if (ma_device_get_state(&gAudio.state.device) ==
      ma_device_state_uninitialized) {
    gAudio.deviceReady = false;
    invalidatePlaybackDeviceRuntimeState();
    gAudio.state.deviceRecoveryPending.store(false, std::memory_order_relaxed);
    gAudio.state.deviceStopExpected.store(false, std::memory_order_relaxed);
    return;
  }

  gAudio.state.deviceStopExpected.store(true, std::memory_order_relaxed);
  // Miniaudio explicitly allows uninit without a prior stop when the backend
  // may have drifted out of sync after OS-level device changes.
  ma_device_uninit(&gAudio.state.device);
  gAudio.deviceReady = false;
  invalidatePlaybackDeviceRuntimeState();
  gAudio.state.deviceRecoveryPending.store(false, std::memory_order_relaxed);
  gAudio.state.deviceStopExpected.store(false, std::memory_order_relaxed);
}

static bool ensurePlaybackDeviceRunningInternalUnlocked() {
  if (!gAudio.deviceReady) {
    return initPlaybackDeviceUnlocked();
  }

  if (ma_device_get_state(&gAudio.state.device) ==
          ma_device_state_uninitialized ||
      gAudio.state.deviceRecoveryPending.load(std::memory_order_relaxed)) {
    uninitPlaybackDeviceUnlocked();
    return initPlaybackDeviceUnlocked();
  }

  const ma_device_state deviceState = ma_device_get_state(&gAudio.state.device);
  if (deviceState == ma_device_state_started ||
      deviceState == ma_device_state_starting) {
    updatePlaybackDeviceDelayFramesUnlocked();
    return true;
  }

  if (ma_device_start(&gAudio.state.device) != MA_SUCCESS) {
    uninitPlaybackDeviceUnlocked();
    return initPlaybackDeviceUnlocked();
  }

  gAudio.state.deviceStopExpected.store(false, std::memory_order_relaxed);
  gAudio.state.deviceRecoveryPending.store(false, std::memory_order_relaxed);
  updatePlaybackDeviceDelayFramesUnlocked();
  return true;
}

}  // namespace

bool audioPlaybackDeviceEnsureRunning() {
  std::lock_guard<std::mutex> lock(gPlaybackDeviceMutex);
  return ensurePlaybackDeviceRunningInternalUnlocked();
}

bool audioPlaybackDeviceRecreate() {
  std::lock_guard<std::mutex> lock(gPlaybackDeviceMutex);
  uninitPlaybackDeviceUnlocked();
  return initPlaybackDeviceUnlocked();
}

void audioPlaybackDeviceUninit() {
  std::lock_guard<std::mutex> lock(gPlaybackDeviceMutex);
  uninitPlaybackDeviceUnlocked();
}

uint64_t audioPlaybackDeviceLatencyFrames() {
  std::lock_guard<std::mutex> lock(gPlaybackDeviceMutex);
  updatePlaybackDeviceDelayFramesUnlocked();
  return gAudio.state.deviceDelayFrames.load(std::memory_order_relaxed);
}

void audioPlaybackDeviceFillPerfStats(AudioPerfStats* stats) {
  if (!stats) return;
  std::lock_guard<std::mutex> lock(gPlaybackDeviceMutex);
  if (!gAudio.deviceReady ||
      ma_device_get_state(&gAudio.state.device) ==
          ma_device_state_uninitialized) {
    return;
  }

  stats->periodFrames = gAudio.state.device.playback.internalPeriodSizeInFrames;
  stats->periods = gAudio.state.device.playback.internalPeriods;
  stats->bufferFrames = stats->periodFrames * stats->periods;
}
