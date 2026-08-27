#include "audioplayback.h"
#include "app_common.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "clock.h"
#include "audio_format_options.h"
#include "audio_stream.h"
#include "melodyanalysiscache.h"
#include "pipeline_transition.h"
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

bool audioPlaybackFinishSeekPipelineTransition(AudioState& state) {
  if (audioPipelineTransitionFinishCommit(state.pipelineTransition)) {
    state.seekRequested.store(false, std::memory_order_release);
    return true;
  }
  return false;
}

static std::chrono::steady_clock::duration playbackPipelineDrainBudget(
    const AudioState& state) {
  const uint32_t sampleRate = state.sampleRate;
  assert(sampleRate > 0);
  const uint32_t transitionFrames = audioPipelineTransitionFrames(sampleRate);
  const uint32_t callbackFrames =
      state.lastCallbackFrames.load(std::memory_order_relaxed);
  const uint64_t observedCallbackFrames =
      callbackFrames > 0 ? callbackFrames : transitionFrames;
  const uint64_t waitFrames =
      static_cast<uint64_t>(transitionFrames) + observedCallbackFrames * 4u;
  const uint64_t waitUs =
      (waitFrames * 1000000ull + sampleRate - 1u) / sampleRate;
  return std::chrono::microseconds(waitUs);
}

void drainPlaybackPipelineForReplacement(AudioPlaybackState& audio) {
  AudioState& state = audio.state;
  if (!audio.deviceReady ||
      !state.audioPrimed.load(std::memory_order_relaxed)) {
    return;
  }
  if (state.paused.load(std::memory_order_relaxed) ||
      state.hold.load(std::memory_order_relaxed)) {
    return;
  }

  queuedAudioSourceCancelStreamResets(&state);
  audioPipelineTransitionRequestDiscontinuity(state.pipelineTransition,
                                              state.sampleRate);
  state.audioQueueCv.notify_all();
  state.radioDspCv.notify_all();

  const auto deadline =
      std::chrono::steady_clock::now() + playbackPipelineDrainBudget(state);
  while (std::chrono::steady_clock::now() < deadline) {
    if (audioPipelineTransitionBeginCommit(state.pipelineTransition)) {
      return;
    }
    if (!audioPipelineTransitionActive(state.pipelineTransition)) {
      return;
    }
    std::unique_lock<std::mutex> lock(state.audioQueueMutex);
    state.audioQueueCv.wait_until(lock, deadline);
  }
  audioPipelineTransitionBeginCommit(state.pipelineTransition);
}

void stopAndUninitActiveDecoder(AudioPlaybackState& audio) {
  queuedAudioSourceStopDecoderWorker(&audio.state);
  if (audio.decoderReady) {
    const AudioBackendHandlers* backend = audio.state.backend;
    if (backend && backend->uninit) {
      backend->uninit(audio);
    }
  }
  queuedAudioSourceStopProcessing(&audio.state);
  audio.decoderReady = false;
  audio.state.backend = nullptr;
  audio.state.mode.store(AudioMode::None, std::memory_order_relaxed);
  audio.state.externalStream.store(false);
  audio.state.streamSerial.store(0, std::memory_order_relaxed);
  audio.state.streamClockReady.store(false, std::memory_order_relaxed);
  audio.state.streamStarved.store(false, std::memory_order_relaxed);
  audio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  audio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  audio.state.processedAtEnd.store(false, std::memory_order_relaxed);
  audio.state.audioClock.reset(0);
  audio.state.audioQueueCv.notify_all();
}

static void releasePlaybackPipelineForNewSignal(AudioState& state) {
  audioPipelineTransitionReset(state.pipelineTransition);
  audioPipelineTransitionRequestSignalFadeIn(state.pipelineTransition,
                                             state.sampleRate);
}

void resetPlaybackStateForLoad(AudioPlaybackState& audio, uint64_t startFrame,
                               bool releasePipelineTransition) {
  audio.state.framesPlayed.store(startFrame);
  audio.state.audioClockFrames.store(startFrame);
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
  audio.state.audioPaddingFrames.store(0);
  audio.state.audioTrailingPaddingFrames.store(0);
  audio.state.audioLeadSilenceFrames.store(0);
  audio.state.streamQueueEnabled.store(false, std::memory_order_relaxed);
  audio.state.streamSerial.store(0, std::memory_order_relaxed);
  audio.state.streamClockReady.store(false, std::memory_order_relaxed);
  audio.state.streamStarved.store(false, std::memory_order_relaxed);
  audio.state.streamDiscardPtsUs.store(0, std::memory_order_relaxed);
  audio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  audio.state.processedAtEnd.store(false, std::memory_order_relaxed);
  audio.state.deviceDelayFrames.store(0, std::memory_order_relaxed);
  if (releasePipelineTransition) {
    releasePlaybackPipelineForNewSignal(audio.state);
  }

  audio.state.channels = audio.channels;
}

bool loadFileAt(AudioPlaybackState& audio, const std::filesystem::path& file,
                uint64_t startFrame, int trackIndex) {
  if (!validateSupportedAudioInputFile(file, &audio.lastInitError)) {
    return false;
  }
  melodyOfflineStop();
  audio.state.audioLeadSilenceFrames.store(0);
  if (audio.audition.active.load()) {
    stopAuditionWorker(audio);
    audio.audition.resumeValid = false;
  }

  audio.lastInitError.clear();
  audio.gmeWarning.clear();
  audio.gsfWarning.clear();
  audio.vgmWarning.clear();

  const AudioBackendHandlers* backend = selectAudioBackend(file);
  if (!backend) {
    audio.lastInitError = "Unsupported audio format.";
    return false;
  }

  drainPlaybackPipelineForReplacement(audio);
  audio.state.sourcePreparing.store(true, std::memory_order_release);
  stopAndUninitActiveDecoder(audio);
  resetPlaybackStateForLoad(audio, startFrame, true);

  const PlaybackSourcePriming priming =
      playbackSourcePrimingForRate(audio.sampleRate);
  const bool usesDecoderWorker =
      queuedAudioSourceUsesDecoderWorker(backend);
  if (!usesDecoderWorker) {
    queuedAudioSourceStartProcessing(
        &audio.state, priming.capacityFrames, priming.targetFrames,
        priming.primeFrames, startFrame, 0);
  }

  if (!openDecoderForBackend(audio, backend, file, startFrame, trackIndex)) {
    queuedAudioSourceStopProcessing(&audio.state);
    audio.state.sourcePreparing.store(false, std::memory_order_release);
    audioPipelineTransitionReset(audio.state.pipelineTransition);
    return false;
  }

  seekLoadedDecoderToStart(audio, backend, &startFrame);
  if (usesDecoderWorker) {
    resetPlaybackStateForLoad(audio, startFrame, false);
    queuedAudioSourceStartProcessing(
        &audio.state, priming.capacityFrames, priming.targetFrames,
        priming.primeFrames, startFrame, 0);
    if (!queuedAudioSourceStartDecoderWorker(audio, backend, startFrame)) {
      uninitOpenedDecoder(audio, backend);
      queuedAudioSourceStopProcessing(&audio.state);
      audio.lastInitError = "Failed to start audio source decoder.";
      audio.state.sourcePreparing.store(false, std::memory_order_release);
      audioPipelineTransitionReset(audio.state.pipelineTransition);
      return false;
    }
  }

  if (!queuedAudioSourceWaitPrimed(&audio.state, priming.primeFrames)) {
    queuedAudioSourceStopDecoderWorker(&audio.state);
    uninitOpenedDecoder(audio, backend);
    queuedAudioSourceStopProcessing(&audio.state);
    audio.lastInitError = "Failed to prime audio source.";
    audio.state.sourcePreparing.store(false, std::memory_order_release);
    audioPipelineTransitionReset(audio.state.pipelineTransition);
    return false;
  }
  activateBackend(audio, backend, trackIndex);
  audio.state.sourcePreparing.store(false, std::memory_order_release);
  audioPipelineTransitionRequestOutputFadeIn(
      audio.state.pipelineTransition, audio.state.sampleRate);

  if (!audioPlaybackDeviceEnsureRunning(audio)) {
    audio.state.sourcePreparing.store(false, std::memory_order_release);
    stopAndUninitActiveDecoder(audio);
    audioPipelineTransitionReset(audio.state.pipelineTransition);
    return false;
  }

  const uint64_t analysisLeadInFrames = audio.state.audioLeadSilenceFrames.load();
  if (backend->allowConcurrentOfflineAnalysis) {
    melodyOfflineStart(file, trackIndex, audio.sampleRate, audio.channels,
                      analysisLeadInFrames, audio.kssOptions,
                      audio.nsfOptions, audio.vgmOptions,
                      audio.vgmDeviceOverrides);
  }

  audio.nowPlaying = file;
  return true;
}

void stopPlayback(AudioPlaybackState& audio) {
  if (audio.audition.active.load()) {
    stopAuditionWorker(audio);
    audio.audition.resumeValid = false;
  }
  drainPlaybackPipelineForReplacement(audio);
  audioPlaybackDeviceUninit(audio);
  if (audio.decoderReady) {
    stopAndUninitActiveDecoder(audio);
  }
  audio.state.framesPlayed.store(0);
  audio.state.audioClockFrames.store(0);
  audio.state.finished.store(false);
  audio.state.seekRequested.store(false);
  audio.state.pendingSeekFrames.store(0);
  audio.state.seekPresentation.reset();
  audio.state.sourcePreparing.store(false, std::memory_order_release);
  audioPipelineTransitionReset(audio.state.pipelineTransition);
  audio.state.audioPrimed.store(false);
  audio.state.paused.store(false);
  audio.state.hold.store(false);
  audio.state.externalStream.store(false);
  audio.state.streamQueueEnabled.store(false);
  audio.state.streamSerial.store(0);
  audio.state.streamClockReady.store(false);
  audio.state.streamStarved.store(false);
  audio.state.processedAtEnd.store(false);
  audio.state.audioClock.reset(0);
  melodyOfflineStop();
  audio.nowPlaying.clear();
  audio.trackIndex = 0;
  audio.lastInitError.clear();
  audio.gmeWarning.clear();
  audio.gsfWarning.clear();
  audio.vgmWarning.clear();
}
AudioPlaybackRuntime::AudioPlaybackRuntime(
    const AudioPlaybackConfig& config) {
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

AudioPlaybackRuntime::~AudioPlaybackRuntime() {
  stopPlayback(gAudio);
  audioPlaybackDeviceUninit(gAudio);
}

bool AudioPlaybackRuntime::enabled() const { return audioIsEnabled(); }

bool AudioPlaybackRuntime::ready() const { return audioIsReady(); }

bool AudioPlaybackRuntime::startFile(const std::filesystem::path& file,
                                     int trackIndex) {
  return audioStartFile(file, trackIndex);
}

void AudioPlaybackRuntime::stop() { audioStop(); }

AudioPlaybackSnapshot AudioPlaybackRuntime::snapshot() const {
  return audioGetPlaybackSnapshot();
}

AudioPerfStats AudioPlaybackRuntime::perfStats() const {
  return audioGetPerfStats();
}

bool AudioPlaybackRuntime::startStream(uint64_t totalFrames) {
  return audioStartStream(gAudio, totalFrames);
}

void AudioPlaybackRuntime::stopStream() { audioStopStream(gAudio); }

size_t AudioPlaybackRuntime::streamBufferedFrames() const {
  return audioStreamBufferedFrames(gAudio);
}

size_t AudioPlaybackRuntime::streamCapacityFrames() const {
  return audioStreamCapacityFrames(gAudio);
}

int64_t AudioPlaybackRuntime::streamOldestPtsUs() const {
  return audioStreamOldestPtsUs(gAudio);
}

bool AudioPlaybackRuntime::writeStreamSamples(
    const float* interleaved, uint64_t frames, int64_t ptsUs, int serial,
    bool allowBlock, uint64_t* writtenFrames) {
  return audioStreamWriteSamples(gAudio, interleaved, frames, ptsUs, serial,
                                 allowBlock, writtenFrames);
}

void AudioPlaybackRuntime::primeStreamClock(int serial,
                                            int64_t targetPtsUs) {
  audioStreamPrimeClock(gAudio, serial, targetPtsUs);
}

void AudioPlaybackRuntime::flushStreamSerial(int serial,
                                             int64_t discardUntilUs) {
  audioStreamFlushSerial(gAudio, serial, discardUntilUs);
}

void AudioPlaybackRuntime::resetStream(uint64_t framePosition) {
  audioStreamReset(gAudio, framePosition);
}

AudioStreamReset AudioPlaybackRuntime::lastAppliedStreamReset() const {
  return audioStreamLastAppliedReset(gAudio);
}

void AudioPlaybackRuntime::setStreamEnd(bool atEnd) {
  audioStreamSetEnd(gAudio, atEnd);
}

int AudioPlaybackRuntime::streamSerial() const {
  return audioStreamSerial(gAudio);
}

int64_t AudioPlaybackRuntime::streamClockUs(int64_t nowUs) const {
  return audioStreamClockUs(gAudio, nowUs);
}

int64_t AudioPlaybackRuntime::streamClockLastUpdatedUs() const {
  return audioStreamClockLastUpdatedUs(gAudio);
}

bool AudioPlaybackRuntime::streamClockReady() const {
  return audioStreamClockReady(gAudio);
}

bool AudioPlaybackRuntime::streamStarved() const {
  return audioStreamStarved(gAudio);
}

uint64_t AudioPlaybackRuntime::streamUpdateCounter() const {
  return audioStreamUpdateCounter(gAudio);
}

uint64_t AudioPlaybackRuntime::waitForStreamUpdate(uint64_t lastCounter,
                                                   int timeoutMs) const {
  return audioStreamWaitForUpdate(gAudio, lastCounter, timeoutMs);
}

void AudioPlaybackRuntime::setHold(bool hold) { audioSetHold(hold); }

void AudioPlaybackRuntime::play() { audioPlay(); }

void AudioPlaybackRuntime::pause() { audioPause(); }

void AudioPlaybackRuntime::togglePause() { audioTogglePause(); }

void AudioPlaybackRuntime::seekBy(int direction) { audioSeekBy(direction); }

void AudioPlaybackRuntime::seekToRatio(double ratio) {
  audioSeekToRatio(ratio);
}

void AudioPlaybackRuntime::cycleRadioFilter() { audioCycleRadioFilter(); }

void AudioPlaybackRuntime::toggle50Hz() { audioToggle50Hz(gAudio); }

void AudioPlaybackRuntime::adjustVolume(float delta) {
  audioAdjustVolume(delta);
}

RadioFilterMode AudioPlaybackRuntime::radioFilterMode() const {
  return audioGetRadioFilterMode();
}

bool AudioPlaybackRuntime::radioEnabled() const {
  return audioIsRadioEnabled();
}

bool AudioPlaybackRuntime::supports50HzToggle() const {
  return audioSupports50HzToggle(gAudio);
}

std::string AudioPlaybackRuntime::warning() const { return audioGetWarning(); }

AudioMelodyInfo AudioPlaybackRuntime::melodyInfo() const {
  return audioGetMelodyInfo();
}

AudioMelodyAnalysisState AudioPlaybackRuntime::melodyAnalysisState() const {
  return audioGetMelodyAnalysisState();
}

bool AudioPlaybackRuntime::analyzeFileToMelodyFile(
    const std::filesystem::path& file, int trackIndex,
    const std::filesystem::path& outputFile,
    const std::function<void(float)>& progressCallback,
    std::string* error) const {
  return audioAnalyzeFileToMelodyFile(file, trackIndex, outputFile,
                                      progressCallback, error);
}

bool AudioPlaybackRuntime::canAnalyzeFile(
    const std::filesystem::path& file) const {
  return audioCanAnalyzeFileToMelodyFile(file);
}

KssPlaybackOptions AudioPlaybackRuntime::kssOptions() const {
  return audioGetKssOptionState(gAudio);
}

bool AudioPlaybackRuntime::kssInstrumentAuditionState(
    KssInstrumentDevice* device, uint32_t* hash) const {
  return audioGetKssInstrumentAuditionState(gAudio, device, hash);
}

bool AudioPlaybackRuntime::startKssInstrumentAudition(
    const KssInstrumentProfile& profile) {
  return audioStartKssInstrumentAudition(gAudio, profile);
}

bool AudioPlaybackRuntime::stopKssInstrumentAudition() {
  return audioStopKssInstrumentAudition(gAudio);
}

bool AudioPlaybackRuntime::adjustKssOption(KssOptionId id, int direction) {
  return audioAdjustKssOption(gAudio, id, direction);
}

NsfPlaybackOptions AudioPlaybackRuntime::nsfOptions() const {
  return audioGetNsfOptionState(gAudio);
}

bool AudioPlaybackRuntime::adjustNsfOption(NsfOptionId id, int direction) {
  return audioAdjustNsfOption(gAudio, id, direction);
}

VgmPlaybackOptions AudioPlaybackRuntime::vgmOptions() const {
  return audioGetVgmOptionState(gAudio);
}

bool AudioPlaybackRuntime::adjustVgmOption(VgmOptionId id, int direction) {
  return audioAdjustVgmOption(gAudio, id, direction);
}

bool AudioPlaybackRuntime::vgmDeviceOptions(uint32_t deviceId,
                                            VgmDeviceOptions* out) const {
  return audioGetVgmDeviceOptions(gAudio, deviceId, out);
}

bool AudioPlaybackRuntime::adjustVgmDeviceOption(
    const VgmDeviceInfo& device, const VgmDeviceOptions& baseline,
    VgmDeviceOptionId id, int direction) {
  return audioAdjustVgmDeviceOption(gAudio, device, baseline, id, direction);
}

bool audioIsEnabled() { return gAudio.enableAudio; }

bool audioIsReady() { return gAudio.decoderReady; }

bool audioStartFile(const std::filesystem::path& file, int trackIndex) {
  return loadFileAt(gAudio, file, 0, trackIndex);
}

bool audioStartFileAt(const std::filesystem::path& file, double startSec,
                      int trackIndex) {
  if (!std::isfinite(startSec) || startSec <= 0.0) {
    return loadFileAt(gAudio, file, 0, trackIndex);
  }
  double framesDouble = startSec * static_cast<double>(gAudio.sampleRate);
  int64_t startFrame = static_cast<int64_t>(std::llround(framesDouble));
  if (startFrame < 0) startFrame = 0;
  return loadFileAt(gAudio, file, static_cast<uint64_t>(startFrame),
                    trackIndex);
}

void audioStop() {
  gAudio.audition.resumeValid = false;
  stopPlayback(gAudio);
}

static void requestAudioSeekFrame(int64_t target) {
  if (target < 0) target = 0;
  const uint64_t total = gAudio.state.totalFrames.load();
  if (total > 0 && target > static_cast<int64_t>(total)) {
    target = static_cast<int64_t>(total);
  }
  gAudio.state.pendingSeekFrames.store(target);
  gAudio.state.seekRequested.store(true, std::memory_order_release);
  gAudio.state.seekPresentation.request();
  gAudio.state.finished.store(false);
  gAudio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.processedAtEnd.store(false, std::memory_order_relaxed);
  gAudio.state.audioPrimed.store(false);
  audioPipelineTransitionRequestDiscontinuity(gAudio.state.pipelineTransition,
                                              gAudio.state.sampleRate);
  gAudio.state.audioQueueCv.notify_all();
  gAudio.state.radioDspCv.notify_all();
}

std::optional<AudioPlaybackSource> audioGetPlaybackSource() {
  if (gAudio.nowPlaying.empty()) return std::nullopt;

  AudioPlaybackSource source;
  source.file = gAudio.nowPlaying;
  const AudioBackendHandlers* backend = gAudio.state.backend;
  if (gAudio.decoderReady && backend && backend->supportsTrackIndex) {
    source.trackIndex = gAudio.trackIndex;
  }
  return source;
}

AudioPlaybackSnapshot audioGetPlaybackSnapshot() {
  AudioPlaybackSnapshot snapshot;
  snapshot.source = audioGetPlaybackSource();
  snapshot.ready = audioIsReady();
  snapshot.seeking = audioIsSeeking();
  snapshot.paused = audioIsPaused();
  snapshot.finished = audioIsFinished();
  snapshot.streamClockReady = audioStreamClockReady(gAudio);
  snapshot.streamStarved = audioStreamStarved(gAudio);
  snapshot.holding = audioIsHolding();
  snapshot.radioEnabled = audioIsRadioEnabled();
  snapshot.hz50Enabled = audioIs50HzEnabled(gAudio);
  snapshot.supports50HzToggle = audioSupports50HzToggle(gAudio);
  snapshot.positionSec = snapshot.ready ? audioGetTimeSec() : 0.0;
  snapshot.durationSec = snapshot.ready ? audioGetTotalSec() : -1.0;
  snapshot.seekTargetSec =
      snapshot.seeking ? audioGetSeekTargetSec() : -1.0;
  snapshot.volume = audioGetVolume();
  snapshot.unclippedOutputPeak = audioGetUnclippedOutputPeak();
  snapshot.radioFilterLabel = audioGetRadioFilterLabel();
  return snapshot;
}

double audioGetTimeSec() {
  if (!gAudio.decoderReady) {
    return 0.0;
  }
  if (!gAudio.state.audioPrimed.load()) {
    return 0.0;
  }
  int64_t frames = static_cast<int64_t>(gAudio.state.audioClockFrames.load());
  uint64_t latencyFrames = audioPlaybackDeviceLatencyFrames(gAudio);
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
  return gAudio.state.seekRequested.load(std::memory_order_acquire) ||
         gAudio.state.seekPresentation.pending();
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
  const AudioMode mode = currentAudioMode(gAudio);
  stats.usingFfmpeg = mode == AudioMode::M4a || mode == AudioMode::Ffmpeg;
  audioPlaybackDeviceFillPerfStats(gAudio, &stats);
  return stats;
}

void audioPause() {
  if (!gAudio.decoderReady) return;
  if (gAudio.state.finished.load(std::memory_order_relaxed)) return;
  gAudio.state.paused.store(true, std::memory_order_relaxed);
}

void audioPlay() {
  if (!gAudio.decoderReady) return;
  const bool wasPaused =
      gAudio.state.paused.load(std::memory_order_relaxed);
  const bool wasFinished =
      gAudio.state.finished.load(std::memory_order_relaxed);
  if (!wasPaused && !wasFinished) return;

  // The device owner recreates only when Windows reported a stopped, rerouted,
  // interrupted, or otherwise invalid endpoint.
  if (!audioPlaybackDeviceEnsureRunning(gAudio)) {
    gAudio.lastInitError = "Failed to restore audio output device.";
    return;
  }

  gAudio.lastInitError.clear();
  gAudio.state.paused.store(false, std::memory_order_relaxed);
  const bool externalStream =
      gAudio.state.externalStream.load(std::memory_order_relaxed);
  if (wasFinished && !externalStream) {
    requestAudioSeekFrame(0);
    return;
  }

  gAudio.state.finished.store(false, std::memory_order_relaxed);
  gAudio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  if (externalStream) {
    gAudio.state.streamClockReady.store(false, std::memory_order_relaxed);
    gAudio.state.streamStarved.store(false, std::memory_order_relaxed);
    gAudio.state.audioClock.reset(
        gAudio.state.streamSerial.load(std::memory_order_relaxed));
  }
  audioPipelineTransitionRequestOutputFadeIn(gAudio.state.pipelineTransition,
                                             gAudio.state.sampleRate);
}

void audioTogglePause() {
  if (!gAudio.decoderReady) return;
  if (gAudio.state.paused.load(std::memory_order_relaxed) ||
      gAudio.state.finished.load(std::memory_order_relaxed)) {
    audioPlay();
  } else {
    audioPause();
  }
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

float audioGetUnclippedOutputPeak() {
  return gAudio.state.unclippedOutputPeak.load(std::memory_order_relaxed);
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
  std::string warning = warningForBackend(gAudio, gAudio.state.backend);
  if (!warning.empty()) return warning;
  return gAudio.lastInitError;
}
