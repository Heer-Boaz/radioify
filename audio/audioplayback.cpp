#include "audioplayback.h"
#include "app_common.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "clock.h"
#include "melodyanalysiscache.h"
#include "pipeline_transition.h"
#include "output_volume_safety.h"
#include "playback_backend.h"
#include "playback_device.h"
#include "playback_source_priming.h"
#include "queued_audio_source.h"
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
  std::ofstream f(radioifyLogPath(), std::ios::app);
  if (f) f << radioifyLogTimestamp() << " " << line << "\n";
#else
  (void)line;
#endif
}

bool audioPlaybackFinishSeekPipelineTransition(AudioState* state) {
  if (!state) return false;
  if (audioPipelineTransitionFinishCommit(state->pipelineTransition)) {
    state->seekRequested.store(false, std::memory_order_relaxed);
    return true;
  }
  return false;
}

static std::chrono::steady_clock::duration playbackPipelineDrainBudget(
    const AudioState* state) {
  const uint32_t sampleRate = std::max(1u, state ? state->sampleRate : 48000u);
  const uint32_t transitionFrames = audioPipelineTransitionFrames(sampleRate);
  const uint32_t callbackFrames =
      state ? state->lastCallbackFrames.load(std::memory_order_relaxed) : 0;
  const uint64_t observedCallbackFrames =
      callbackFrames > 0 ? callbackFrames : transitionFrames;
  const uint64_t waitFrames =
      static_cast<uint64_t>(transitionFrames) + observedCallbackFrames * 4u;
  const uint64_t waitUs =
      (waitFrames * 1000000ull + sampleRate - 1u) / sampleRate;
  return std::chrono::microseconds(waitUs);
}

void drainPlaybackPipelineForReplacement(AudioState* state) {
  if (!state || !gAudio.deviceReady ||
      !state->audioPrimed.load(std::memory_order_relaxed)) {
    return;
  }
  if (state->paused.load(std::memory_order_relaxed) ||
      state->hold.load(std::memory_order_relaxed)) {
    return;
  }

  queuedAudioSourceCancelStreamResets(state);
  audioPipelineTransitionRequestDiscontinuity(state->pipelineTransition,
                                              state->sampleRate);
  state->audioQueueCv.notify_all();
  state->radioDspCv.notify_all();

  const auto deadline =
      std::chrono::steady_clock::now() + playbackPipelineDrainBudget(state);
  while (std::chrono::steady_clock::now() < deadline) {
    if (audioPipelineTransitionBeginCommit(state->pipelineTransition)) {
      return;
    }
    if (!audioPipelineTransitionActive(state->pipelineTransition)) {
      return;
    }
    std::unique_lock<std::mutex> lock(state->audioQueueMutex);
    state->audioQueueCv.wait_until(lock, deadline);
  }
  audioPipelineTransitionBeginCommit(state->pipelineTransition);
}

void stopAndUninitActiveDecoder() {
  queuedAudioSourceStopDecoderWorker(&gAudio.state);
  if (gAudio.decoderReady) {
    const AudioBackendHandlers* backend = gAudio.state.backend;
    if (backend && backend->uninit) {
      backend->uninit();
    }
  }
  queuedAudioSourceStopProcessing(&gAudio.state);
  gAudio.decoderReady = false;
  gAudio.state.backend = nullptr;
  gAudio.state.mode.store(AudioMode::None, std::memory_order_relaxed);
  gAudio.state.externalStream.store(false);
  gAudio.state.streamSerial.store(0, std::memory_order_relaxed);
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.processedAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.audioClock.reset(0);
  gAudio.state.audioQueueCv.notify_all();
}

static void releasePlaybackPipelineForNewSignal() {
  audioPipelineTransitionReset(gAudio.state.pipelineTransition);
  audioPipelineTransitionRequestSignalFadeIn(gAudio.state.pipelineTransition,
                                             gAudio.state.sampleRate);
}

void resetPlaybackStateForLoad(uint64_t startFrame,
                               bool releasePipelineTransition) {
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
  gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
  gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
  gAudio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  gAudio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.processedAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.deviceDelayFrames.store(0, std::memory_order_relaxed);
  gAudio.state.outputSafety = {};
  if (releasePipelineTransition) {
    releasePlaybackPipelineForNewSignal();
  }

  gAudio.state.channels = gAudio.channels;
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

  drainPlaybackPipelineForReplacement(&gAudio.state);
  gAudio.state.sourcePreparing.store(true, std::memory_order_release);
  stopAndUninitActiveDecoder();
  resetPlaybackStateForLoad(startFrame, true);

  const PlaybackSourcePriming priming =
      playbackSourcePrimingForRate(gAudio.sampleRate);
  const bool usesDecoderWorker =
      queuedAudioSourceUsesDecoderWorker(backend);
  if (!usesDecoderWorker) {
    queuedAudioSourceStartProcessing(
        &gAudio.state, priming.capacityFrames, priming.targetFrames,
        priming.primeFrames, startFrame, 0);
  }

  if (!openDecoderForBackend(backend, file, startFrame, trackIndex)) {
    queuedAudioSourceStopProcessing(&gAudio.state);
    gAudio.state.sourcePreparing.store(false, std::memory_order_release);
    audioPipelineTransitionReset(gAudio.state.pipelineTransition);
    return false;
  }

  seekLoadedDecoderToStart(backend, &startFrame);
  if (usesDecoderWorker) {
    resetPlaybackStateForLoad(startFrame, false);
    queuedAudioSourceStartProcessing(
        &gAudio.state, priming.capacityFrames, priming.targetFrames,
        priming.primeFrames, startFrame, 0);
    if (!queuedAudioSourceStartDecoderWorker(backend, startFrame)) {
      uninitOpenedDecoder(backend);
      queuedAudioSourceStopProcessing(&gAudio.state);
      gAudio.lastInitError = "Failed to start audio source decoder.";
      gAudio.state.sourcePreparing.store(false, std::memory_order_release);
      audioPipelineTransitionReset(gAudio.state.pipelineTransition);
      return false;
    }
  }

  if (!queuedAudioSourceWaitPrimed(&gAudio.state, priming.primeFrames)) {
    queuedAudioSourceStopDecoderWorker(&gAudio.state);
    uninitOpenedDecoder(backend);
    queuedAudioSourceStopProcessing(&gAudio.state);
    gAudio.lastInitError = "Failed to prime audio source.";
    gAudio.state.sourcePreparing.store(false, std::memory_order_release);
    audioPipelineTransitionReset(gAudio.state.pipelineTransition);
    return false;
  }
  activateBackend(backend, trackIndex);
  gAudio.state.sourcePreparing.store(false, std::memory_order_release);
  audioPipelineTransitionRequestOutputFadeIn(
      gAudio.state.pipelineTransition, gAudio.state.sampleRate);

  if (!audioPlaybackDeviceEnsureRunning()) {
    gAudio.state.sourcePreparing.store(false, std::memory_order_release);
    stopAndUninitActiveDecoder();
    audioPipelineTransitionReset(gAudio.state.pipelineTransition);
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

  drainPlaybackPipelineForReplacement(&gAudio.state);
  audioPlaybackDeviceUninit();
  if (gAudio.decoderReady) {
    stopAndUninitActiveDecoder();
  }

  gAudio.channels = newChannels;
  gAudio.state.channels = gAudio.channels;

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
  drainPlaybackPipelineForReplacement(&gAudio.state);
  audioPlaybackDeviceUninit();
  if (gAudio.decoderReady) {
    stopAndUninitActiveDecoder();
  }
  gAudio.state.framesPlayed.store(0);
  gAudio.state.audioClockFrames.store(0);
  gAudio.state.finished.store(false);
  gAudio.state.seekRequested.store(false);
  gAudio.state.pendingSeekFrames.store(0);
  gAudio.state.sourcePreparing.store(false, std::memory_order_release);
  audioPipelineTransitionReset(gAudio.state.pipelineTransition);
  gAudio.state.audioPrimed.store(false);
  gAudio.state.paused.store(false);
  gAudio.state.hold.store(false);
  gAudio.state.externalStream.store(false);
  gAudio.state.streamQueueEnabled.store(false);
  gAudio.state.streamSerial.store(0);
  gAudio.state.streamClockReady.store(false);
  gAudio.state.streamStarved.store(false);
  gAudio.state.processedAtEnd.store(false);
  gAudio.state.audioClock.reset(0);
  melodyOfflineStop();
  gAudio.nowPlaying.clear();
  gAudio.trackIndex = 0;
  gAudio.lastInitError.clear();
  gAudio.gmeWarning.clear();
  gAudio.gsfWarning.clear();
  gAudio.vgmWarning.clear();
}
void audioInit(const AudioPlaybackConfig& config) {
  gAudio.enableAudio = config.enableAudio;
  gAudio.sampleRate = 48000;
  gAudio.baseChannels = config.mono ? 1u : 2u;
  gAudio.channels = gAudio.baseChannels;
  gAudio.state.channels = gAudio.channels;
  gAudio.state.sampleRate = gAudio.sampleRate;
  gAudio.state.dry = config.dry;
  RadioPlaybackFilterConfig radioFilterConfig;
  radioFilterConfig.sampleRate = gAudio.sampleRate;
  radioFilterConfig.outputChannels = gAudio.channels;
  radioFilterConfig.bandwidthHz = static_cast<float>(config.bwHz);
  radioFilterConfig.noise = static_cast<float>(config.noise);
  radioFilterConfig.initialMode =
      config.enableRadio
          ? radioFilterModeForReceiverProfile(config.radioReceiverProfile)
          : RadioFilterMode::Off;
  radioFilterConfig.reception =
      radioReceptionConfigForProfile(config.radioReceptionProfile);
  radioFilterConfig.settingsPath = config.radioSettingsPath;
  radioFilterConfig.presetName = config.radioPresetName;
  gAudio.state.radioFilter.initialize(radioFilterConfig);
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

static void requestAudioSeekFrame(int64_t target) {
  if (target < 0) target = 0;
  const uint64_t total = gAudio.state.totalFrames.load();
  if (total > 0 && target > static_cast<int64_t>(total)) {
    target = static_cast<int64_t>(total);
  }
  gAudio.state.pendingSeekFrames.store(target);
  gAudio.state.seekRequested.store(true);
  gAudio.state.finished.store(false);
  gAudio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.audioPrimed.store(false);
  audioPipelineTransitionRequestDiscontinuity(gAudio.state.pipelineTransition,
                                              gAudio.state.sampleRate);
  gAudio.state.audioQueueCv.notify_all();
  gAudio.state.radioDspCv.notify_all();
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

bool audioIsRadioEnabled() {
  return radioFilterModeEnabled(gAudio.state.radioFilter.mode());
}

RadioFilterMode audioGetRadioFilterMode() {
  return gAudio.state.radioFilter.mode();
}

std::string_view audioGetRadioFilterLabel() {
  return radioFilterModeLabel(audioGetRadioFilterMode());
}

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
  gAudio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  if (gAudio.state.externalStream.load(std::memory_order_relaxed)) {
    gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
    gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
    gAudio.state.audioClock.reset(
        gAudio.state.streamSerial.load(std::memory_order_relaxed));
  }
  audioPipelineTransitionRequestOutputFadeIn(gAudio.state.pipelineTransition,
                                             gAudio.state.sampleRate);
  gAudio.state.paused.store(false, std::memory_order_relaxed);
}

void audioSeekBy(int direction) {
  if (!gAudio.decoderReady) return;
  int64_t deltaFrames = static_cast<int64_t>(direction) * 5 * gAudio.sampleRate;
  int64_t current = static_cast<int64_t>(gAudio.state.audioClockFrames.load());
  if (gAudio.state.seekRequested.load()) {
    current = gAudio.state.pendingSeekFrames.load();
  }
  requestAudioSeekFrame(current + deltaFrames);
}

void audioSeekToRatio(double ratio) {
  if (!gAudio.decoderReady) return;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total == 0) return;
  ratio = std::clamp(ratio, 0.0, 1.0);
  int64_t target = static_cast<int64_t>(ratio * static_cast<double>(total));
  requestAudioSeekFrame(target);
}

void audioSeekToSec(double sec) {
  if (!gAudio.decoderReady) return;
  if (!std::isfinite(sec)) return;
  int64_t target =
      static_cast<int64_t>(std::llround(sec * gAudio.sampleRate));
  requestAudioSeekFrame(target);
}

void audioCycleRadioFilter() {
  gAudio.state.radioFilter.cycleMode();
  gAudio.state.radioDspCv.notify_all();
}

void audioSetHold(bool hold) {
  const bool wasHold = gAudio.state.hold.exchange(hold);
  if (wasHold && !hold) {
    audioPipelineTransitionRequestOutputFadeIn(gAudio.state.pipelineTransition,
                                               gAudio.state.sampleRate);
  }
}

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

bool audioCanAnalyzeFileToMelodyFile(const std::filesystem::path& file) {
  return melodyOfflineCanAnalyzeFile(file);
}

std::string audioGetWarning() {
  std::string warning = warningForBackend(gAudio.state.backend);
  if (!warning.empty()) return warning;
  return gAudio.lastInitError;
}
