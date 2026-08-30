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
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

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
  audio.nowPlaying.clear();
  audio.trackIndex = 0;
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

audio_playback::FileStartResult loadFileAt(
    AudioPlaybackState& audio, std::filesystem::path file,
    uint64_t startFrame, int trackIndex) {
  if (!validateSupportedAudioInputFile(file, &audio.lastInitError)) {
    return audio_playback::FileStartResult::RejectedPreservingPlayback;
  }
  const AudioBackendHandlers* backend = selectAudioBackend(file);
  if (!backend) {
    audio.lastInitError = "Unsupported audio format.";
    return audio_playback::FileStartResult::RejectedPreservingPlayback;
  }

  // Everything above this boundary is observational: failures must leave the
  // current endpoint intact. From here on the old playback may be replaced.
  audio.melodyAnalysis.stop();
  audio.state.audioLeadSilenceFrames.store(0);
  if (audio.audition.active.load()) {
    stopAuditionWorker(audio);
    audio.audition.resumeValid = false;
  }

  audio.lastInitError.clear();
  audio.gmeWarning.clear();
  audio.gsfWarning.clear();
  audio.vgmWarning.clear();

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
    return audio_playback::FileStartResult::FailedAfterReplacingPlayback;
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
      return audio_playback::FileStartResult::FailedAfterReplacingPlayback;
    }
  }

  if (!queuedAudioSourceWaitPrimed(&audio.state, priming.primeFrames)) {
    queuedAudioSourceStopDecoderWorker(&audio.state);
    uninitOpenedDecoder(audio, backend);
    queuedAudioSourceStopProcessing(&audio.state);
    audio.lastInitError = "Failed to prime audio source.";
    audio.state.sourcePreparing.store(false, std::memory_order_release);
    audioPipelineTransitionReset(audio.state.pipelineTransition);
    return audio_playback::FileStartResult::FailedAfterReplacingPlayback;
  }
  activateBackend(audio, backend, trackIndex);
  audio.state.sourcePreparing.store(false, std::memory_order_release);
  audioPipelineTransitionRequestOutputFadeIn(
      audio.state.pipelineTransition, audio.state.sampleRate);

  if (!audioPlaybackDeviceEnsureRunning(audio)) {
    audio.state.sourcePreparing.store(false, std::memory_order_release);
    stopAndUninitActiveDecoder(audio);
    audioPipelineTransitionReset(audio.state.pipelineTransition);
    return audio_playback::FileStartResult::FailedAfterReplacingPlayback;
  }

  const uint64_t analysisLeadInFrames = audio.state.audioLeadSilenceFrames.load();
  if (backend->allowConcurrentOfflineAnalysis) {
    audio.melodyAnalysis.start(
        file, trackIndex, audio.sampleRate, audio.channels,
        analysisLeadInFrames, audio.kssOptions, audio.nsfOptions,
        audio.vgmOptions, audio.vgmDeviceOverrides);
  }

  audio.nowPlaying = file;
  return audio_playback::FileStartResult::Started;
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
  audio.melodyAnalysis.stop();
  audio.nowPlaying.clear();
  audio.trackIndex = 0;
  audio.lastInitError.clear();
  audio.gmeWarning.clear();
  audio.gsfWarning.clear();
  audio.vgmWarning.clear();
}

static bool audioIsEnabled(const AudioPlaybackState& audio);
static bool audioIsReady(const AudioPlaybackState& audio);
static audio_playback::FileStartResult audioStartFile(
    AudioPlaybackState& audio, const std::filesystem::path& file,
    int trackIndex);
static void audioStop(AudioPlaybackState& audio);
static std::optional<AudioPlaybackSource> audioGetPlaybackSource(
    const AudioPlaybackState& audio);
static AudioPlaybackSnapshot audioGetPlaybackSnapshot(
    AudioPlaybackState& audio);
static double audioGetTimeSec(AudioPlaybackState& audio);
static double audioGetTotalSec(const AudioPlaybackState& audio);
static bool audioIsSeeking(const AudioPlaybackState& audio);
static double audioGetSeekTargetSec(const AudioPlaybackState& audio);
static bool audioIsPaused(const AudioPlaybackState& audio);
static bool audioIsFinished(const AudioPlaybackState& audio);
static bool audioIsRadioEnabled(const AudioPlaybackState& audio);
static RadioFilterMode audioGetRadioFilterMode(
    const AudioPlaybackState& audio);
static std::string_view audioGetRadioFilterLabel(
    const AudioPlaybackState& audio);
static bool audioIsHolding(const AudioPlaybackState& audio);
static AudioPerfStats audioGetPerfStats(AudioPlaybackState& audio);
static void audioPause(AudioPlaybackState& audio);
static void audioPlay(AudioPlaybackState& audio);
static void audioTogglePause(AudioPlaybackState& audio);
static void audioSeekBy(AudioPlaybackState& audio, int direction);
static void audioSeekToRatio(AudioPlaybackState& audio, double ratio);
static void audioCycleRadioFilter(AudioPlaybackState& audio);
static void audioSetHold(AudioPlaybackState& audio, bool hold);
static void audioAdjustVolume(AudioPlaybackState& audio, float delta);
static float audioGetVolume(const AudioPlaybackState& audio);
static float audioGetUnclippedOutputPeak(const AudioPlaybackState& audio);
static AudioMelodyInfo audioGetMelodyInfo(AudioPlaybackState& audio);
static AudioMelodyAnalysisState audioGetMelodyAnalysisState(
    const AudioPlaybackState& audio);
static bool audioAnalyzeFileToMelodyFile(
    const AudioPlaybackState& audio, const std::filesystem::path& file,
    int trackIndex, const std::filesystem::path& outputFile,
    const std::function<void(float)>& progressCallback,
    const std::function<bool()>& cancellationRequested, std::string* error,
    const std::function<bool()>& outputCommitStarted);
static bool audioCanAnalyzeFileToMelodyFile(
    const std::filesystem::path& file);
static std::string audioGetWarning(const AudioPlaybackState& audio);

AudioPlaybackRuntime::AudioPlaybackRuntime(
    const AudioPlaybackConfig& config)
    : state_(std::make_unique<AudioPlaybackState>()) {
  AudioPlaybackState& audio = *state_;
  audio.enableAudio = config.enableAudio;
  audio.sampleRate = 48000;
  audio.baseChannels = config.mono ? 1u : 2u;
  audio.channels = audio.baseChannels;
  audio.state.channels = audio.channels;
  audio.state.sampleRate = audio.sampleRate;
  audio.state.dry = config.dry;
  RadioPlaybackFilterConfig radioFilterConfig;
  radioFilterConfig.sampleRate = audio.sampleRate;
  radioFilterConfig.outputChannels = audio.channels;
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
  audio.state.radioFilter.initialize(radioFilterConfig);
}

AudioPlaybackRuntime::~AudioPlaybackRuntime() {
  stopPlayback(*state_);
}

bool AudioPlaybackRuntime::enabled() const { return audioIsEnabled(*state_); }

bool AudioPlaybackRuntime::ready() const { return audioIsReady(*state_); }

audio_playback::FileStartResult AudioPlaybackRuntime::startFile(
    const std::filesystem::path& file, int trackIndex) {
  return audioStartFile(*state_, file, trackIndex);
}

void AudioPlaybackRuntime::stop() { audioStop(*state_); }

AudioPlaybackSnapshot AudioPlaybackRuntime::snapshot() const {
  return audioGetPlaybackSnapshot(*state_);
}

AudioPerfStats AudioPlaybackRuntime::perfStats() const {
  return audioGetPerfStats(*state_);
}

bool AudioPlaybackRuntime::startStream(uint64_t totalFrames) {
  return audioStartStream(*state_, totalFrames);
}

void AudioPlaybackRuntime::stopStream() { audioStopStream(*state_); }

size_t AudioPlaybackRuntime::streamBufferedFrames() const {
  return audioStreamBufferedFrames(*state_);
}

size_t AudioPlaybackRuntime::streamCapacityFrames() const {
  return audioStreamCapacityFrames(*state_);
}

int64_t AudioPlaybackRuntime::streamOldestPtsUs() const {
  return audioStreamOldestPtsUs(*state_);
}

bool AudioPlaybackRuntime::writeStreamSamples(
    const float* interleaved, uint64_t frames, int64_t ptsUs, int serial,
    bool allowBlock, uint64_t* writtenFrames) {
  return audioStreamWriteSamples(*state_, interleaved, frames, ptsUs, serial,
                                 allowBlock, writtenFrames);
}

void AudioPlaybackRuntime::primeStreamClock(int serial,
                                            int64_t targetPtsUs) {
  audioStreamPrimeClock(*state_, serial, targetPtsUs);
}

void AudioPlaybackRuntime::flushStreamSerial(int serial,
                                             int64_t discardUntilUs) {
  audioStreamFlushSerial(*state_, serial, discardUntilUs);
}

void AudioPlaybackRuntime::resetStream(uint64_t framePosition) {
  audioStreamReset(*state_, framePosition);
}

AudioStreamReset AudioPlaybackRuntime::lastAppliedStreamReset() const {
  return audioStreamLastAppliedReset(*state_);
}

void AudioPlaybackRuntime::setStreamEnd(bool atEnd) {
  audioStreamSetEnd(*state_, atEnd);
}

int AudioPlaybackRuntime::streamSerial() const {
  return audioStreamSerial(*state_);
}

int64_t AudioPlaybackRuntime::streamClockUs(int64_t nowUs) const {
  return audioStreamClockUs(*state_, nowUs);
}

int64_t AudioPlaybackRuntime::streamClockLastUpdatedUs() const {
  return audioStreamClockLastUpdatedUs(*state_);
}

bool AudioPlaybackRuntime::streamClockReady() const {
  return audioStreamClockReady(*state_);
}

bool AudioPlaybackRuntime::streamStarved() const {
  return audioStreamStarved(*state_);
}

uint64_t AudioPlaybackRuntime::streamUpdateCounter() const {
  return audioStreamUpdateCounter(*state_);
}

uint64_t AudioPlaybackRuntime::waitForStreamUpdate(uint64_t lastCounter,
                                                   int timeoutMs) const {
  return audioStreamWaitForUpdate(*state_, lastCounter, timeoutMs);
}

void AudioPlaybackRuntime::setHold(bool hold) { audioSetHold(*state_, hold); }

void AudioPlaybackRuntime::play() { audioPlay(*state_); }

void AudioPlaybackRuntime::pause() { audioPause(*state_); }

void AudioPlaybackRuntime::togglePause() { audioTogglePause(*state_); }

void AudioPlaybackRuntime::seekBy(int direction) {
  audioSeekBy(*state_, direction);
}

void AudioPlaybackRuntime::seekToRatio(double ratio) {
  audioSeekToRatio(*state_, ratio);
}

void AudioPlaybackRuntime::cycleRadioFilter() {
  audioCycleRadioFilter(*state_);
}

void AudioPlaybackRuntime::toggle50Hz() { audioToggle50Hz(*state_); }

void AudioPlaybackRuntime::adjustVolume(float delta) {
  audioAdjustVolume(*state_, delta);
}

RadioFilterMode AudioPlaybackRuntime::radioFilterMode() const {
  return audioGetRadioFilterMode(*state_);
}

bool AudioPlaybackRuntime::radioEnabled() const {
  return audioIsRadioEnabled(*state_);
}

bool AudioPlaybackRuntime::supports50HzToggle() const {
  return audioSupports50HzToggle(*state_);
}

std::string AudioPlaybackRuntime::warning() const {
  return audioGetWarning(*state_);
}

AudioMelodyInfo AudioPlaybackRuntime::melodyInfo() const {
  return audioGetMelodyInfo(*state_);
}

AudioMelodyAnalysisState AudioPlaybackRuntime::melodyAnalysisState() const {
  return audioGetMelodyAnalysisState(*state_);
}

bool AudioPlaybackRuntime::analyzeFileToMelodyFile(
    const std::filesystem::path& file, int trackIndex,
    const std::filesystem::path& outputFile,
    const std::function<void(float)>& progressCallback,
    const std::function<bool()>& cancellationRequested,
    std::string* error,
    const std::function<bool()>& outputCommitStarted) const {
  return audioAnalyzeFileToMelodyFile(*state_, file, trackIndex, outputFile,
                                      progressCallback,
                                      cancellationRequested, error,
                                      outputCommitStarted);
}

bool AudioPlaybackRuntime::canAnalyzeFile(
    const std::filesystem::path& file) const {
  return audioCanAnalyzeFileToMelodyFile(file);
}

KssPlaybackOptions AudioPlaybackRuntime::kssOptions() const {
  return audioGetKssOptionState(*state_);
}

bool AudioPlaybackRuntime::kssInstrumentAuditionState(
    KssInstrumentDevice* device, uint32_t* hash) const {
  return audioGetKssInstrumentAuditionState(*state_, device, hash);
}

bool AudioPlaybackRuntime::startKssInstrumentAudition(
    const KssInstrumentProfile& profile) {
  return audioStartKssInstrumentAudition(*state_, profile);
}

bool AudioPlaybackRuntime::stopKssInstrumentAudition() {
  return audioStopKssInstrumentAudition(*state_);
}

bool AudioPlaybackRuntime::adjustKssOption(KssOptionId id, int direction) {
  return audioAdjustKssOption(*state_, id, direction);
}

NsfPlaybackOptions AudioPlaybackRuntime::nsfOptions() const {
  return audioGetNsfOptionState(*state_);
}

bool AudioPlaybackRuntime::adjustNsfOption(NsfOptionId id, int direction) {
  return audioAdjustNsfOption(*state_, id, direction);
}

VgmPlaybackOptions AudioPlaybackRuntime::vgmOptions() const {
  return audioGetVgmOptionState(*state_);
}

bool AudioPlaybackRuntime::adjustVgmOption(VgmOptionId id, int direction) {
  return audioAdjustVgmOption(*state_, id, direction);
}

bool AudioPlaybackRuntime::vgmDeviceOptions(uint32_t deviceId,
                                            VgmDeviceOptions* out) const {
  return audioGetVgmDeviceOptions(*state_, deviceId, out);
}

bool AudioPlaybackRuntime::adjustVgmDeviceOption(
    const VgmDeviceInfo& device, const VgmDeviceOptions& baseline,
    VgmDeviceOptionId id, int direction) {
  return audioAdjustVgmDeviceOption(*state_, device, baseline, id, direction);
}

static bool audioIsEnabled(const AudioPlaybackState& audio) {
  return audio.enableAudio;
}

static bool audioIsReady(const AudioPlaybackState& audio) {
  return audio.decoderReady;
}

static audio_playback::FileStartResult audioStartFile(
    AudioPlaybackState& audio, const std::filesystem::path& file,
    int trackIndex) {
  return loadFileAt(audio, file, 0, trackIndex);
}

static void audioStop(AudioPlaybackState& audio) {
  audio.audition.resumeValid = false;
  stopPlayback(audio);
}

static void requestAudioSeekFrame(AudioPlaybackState& audio, int64_t target) {
  if (target < 0) target = 0;
  const uint64_t total = audio.state.totalFrames.load();
  if (total > 0 && target > static_cast<int64_t>(total)) {
    target = static_cast<int64_t>(total);
  }
  audio.state.pendingSeekFrames.store(target);
  audio.state.seekRequested.store(true, std::memory_order_release);
  audio.state.seekPresentation.request();
  audio.state.finished.store(false);
  audio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  audio.state.processedAtEnd.store(false, std::memory_order_relaxed);
  audio.state.audioPrimed.store(false);
  audioPipelineTransitionRequestDiscontinuity(audio.state.pipelineTransition,
                                              audio.state.sampleRate);
  audio.state.audioQueueCv.notify_all();
  audio.state.radioDspCv.notify_all();
}

static std::optional<AudioPlaybackSource> audioGetPlaybackSource(
    const AudioPlaybackState& audio) {
  if (audio.nowPlaying.empty()) return std::nullopt;

  AudioPlaybackSource source;
  source.file = audio.nowPlaying;
  const AudioBackendHandlers* backend = audio.state.backend;
  if (audio.decoderReady && backend && backend->supportsTrackIndex) {
    source.trackIndex = audio.trackIndex;
  }
  return source;
}

static AudioPlaybackSnapshot audioGetPlaybackSnapshot(
    AudioPlaybackState& audio) {
  AudioPlaybackSnapshot snapshot;
  snapshot.source = audioGetPlaybackSource(audio);
  snapshot.ready = audioIsReady(audio);
  snapshot.seeking = audioIsSeeking(audio);
  snapshot.paused = audioIsPaused(audio);
  snapshot.finished = audioIsFinished(audio);
  snapshot.streamClockReady = audioStreamClockReady(audio);
  snapshot.streamStarved = audioStreamStarved(audio);
  snapshot.holding = audioIsHolding(audio);
  snapshot.radioEnabled = audioIsRadioEnabled(audio);
  snapshot.hz50Enabled = audioIs50HzEnabled(audio);
  snapshot.supports50HzToggle = audioSupports50HzToggle(audio);
  snapshot.positionSec = snapshot.ready ? audioGetTimeSec(audio) : 0.0;
  snapshot.durationSec = snapshot.ready ? audioGetTotalSec(audio) : -1.0;
  snapshot.seekTargetSec =
      snapshot.seeking ? audioGetSeekTargetSec(audio) : -1.0;
  snapshot.volume = audioGetVolume(audio);
  snapshot.unclippedOutputPeak = audioGetUnclippedOutputPeak(audio);
  snapshot.radioFilterLabel = audioGetRadioFilterLabel(audio);
  return snapshot;
}

static double audioGetTimeSec(AudioPlaybackState& audio) {
  if (!audio.decoderReady) {
    return 0.0;
  }
  if (!audio.state.audioPrimed.load()) {
    return 0.0;
  }
  int64_t frames = static_cast<int64_t>(audio.state.audioClockFrames.load());
  uint64_t latencyFrames = audioPlaybackDeviceLatencyFrames(audio);
  frames -= static_cast<int64_t>(latencyFrames);
  if (frames < 0) {
    frames = 0;
  }
  double timeSec = static_cast<double>(frames) / audio.sampleRate;
  uint64_t totalFrames = audio.state.totalFrames.load();
  if (totalFrames > 0) {
    double totalSec = static_cast<double>(totalFrames) / audio.sampleRate;
    if (audio.state.finished.load() || timeSec > totalSec) {
      timeSec = totalSec;
    }
  }
  return timeSec;
}

static double audioGetTotalSec(const AudioPlaybackState& audio) {
  if (!audio.decoderReady) {
    return -1.0;
  }
  uint64_t total = audio.state.totalFrames.load();
  if (total == 0) return -1.0;
  return static_cast<double>(total) / audio.sampleRate;
}

static bool audioIsSeeking(const AudioPlaybackState& audio) {
  if (!audio.decoderReady) {
    return false;
  }
  return audio.state.seekRequested.load(std::memory_order_acquire) ||
         audio.state.seekPresentation.pending();
}

static double audioGetSeekTargetSec(const AudioPlaybackState& audio) {
  if (!audio.decoderReady) {
    return -1.0;
  }
  uint64_t total = audio.state.totalFrames.load();
  if (total == 0) return -1.0;
  int64_t target = audio.state.pendingSeekFrames.load();
  if (target < 0) target = 0;
  if (static_cast<uint64_t>(target) > total) {
    target = static_cast<int64_t>(total);
  }
  return static_cast<double>(target) / audio.sampleRate;
}

static bool audioIsPaused(const AudioPlaybackState& audio) {
  if (!audio.decoderReady) return false;
  return audio.state.paused.load();
}

static bool audioIsFinished(const AudioPlaybackState& audio) {
  if (!audio.decoderReady) return false;
  return audio.state.finished.load();
}

static bool audioIsRadioEnabled(const AudioPlaybackState& audio) {
  return radioFilterModeEnabled(audio.state.radioFilter.mode());
}

static RadioFilterMode audioGetRadioFilterMode(
    const AudioPlaybackState& audio) {
  return audio.state.radioFilter.mode();
}

static std::string_view audioGetRadioFilterLabel(
    const AudioPlaybackState& audio) {
  return radioFilterModeLabel(audioGetRadioFilterMode(audio));
}

static bool audioIsHolding(const AudioPlaybackState& audio) {
  return audio.state.hold.load();
}

static AudioPerfStats audioGetPerfStats(AudioPlaybackState& audio) {
  AudioPerfStats stats{};
  if (!audio.decoderReady) {
    return stats;
  }
  stats.callbacks = audio.state.callbackCount.load(std::memory_order_relaxed);
  stats.framesRequested =
      audio.state.framesRequested.load(std::memory_order_relaxed);
  stats.framesRead = audio.state.framesReadTotal.load(std::memory_order_relaxed);
  stats.shortReads = audio.state.shortReadCount.load(std::memory_order_relaxed);
  stats.silentFrames = audio.state.silentFrames.load(std::memory_order_relaxed);
  stats.lastCallbackFrames =
      audio.state.lastCallbackFrames.load(std::memory_order_relaxed);
  stats.lastFramesRead =
      audio.state.lastFramesRead.load(std::memory_order_relaxed);
  stats.sampleRate = audio.state.sampleRate;
  stats.channels = audio.state.channels;
  const AudioMode mode = currentAudioMode(audio);
  stats.usingFfmpeg = mode == AudioMode::M4a || mode == AudioMode::Ffmpeg;
  audioPlaybackDeviceFillPerfStats(audio, &stats);
  return stats;
}

static void audioPause(AudioPlaybackState& audio) {
  if (!audio.decoderReady) return;
  if (audio.state.finished.load(std::memory_order_relaxed)) return;
  audio.state.paused.store(true, std::memory_order_relaxed);
}

static void audioPlay(AudioPlaybackState& audio) {
  if (!audio.decoderReady) return;
  const bool wasPaused =
      audio.state.paused.load(std::memory_order_relaxed);
  const bool wasFinished =
      audio.state.finished.load(std::memory_order_relaxed);
  if (!wasPaused && !wasFinished) return;

  // The device owner recreates only when Windows reported a stopped, rerouted,
  // interrupted, or otherwise invalid endpoint.
  if (!audioPlaybackDeviceEnsureRunning(audio)) {
    audio.lastInitError = "Failed to restore audio output device.";
    return;
  }

  audio.lastInitError.clear();
  audio.state.paused.store(false, std::memory_order_relaxed);
  const bool externalStream =
      audio.state.externalStream.load(std::memory_order_relaxed);
  if (wasFinished && !externalStream) {
    requestAudioSeekFrame(audio, 0);
    return;
  }

  audio.state.finished.store(false, std::memory_order_relaxed);
  audio.state.sourceAtEnd.store(false, std::memory_order_relaxed);
  if (externalStream) {
    audio.state.streamClockReady.store(false, std::memory_order_relaxed);
    audio.state.streamStarved.store(false, std::memory_order_relaxed);
    audio.state.audioClock.reset(
        audio.state.streamSerial.load(std::memory_order_relaxed));
  }
  audioPipelineTransitionRequestOutputFadeIn(audio.state.pipelineTransition,
                                             audio.state.sampleRate);
}

static void audioTogglePause(AudioPlaybackState& audio) {
  if (!audio.decoderReady) return;
  if (audio.state.paused.load(std::memory_order_relaxed) ||
      audio.state.finished.load(std::memory_order_relaxed)) {
    audioPlay(audio);
  } else {
    audioPause(audio);
  }
}

static void audioSeekBy(AudioPlaybackState& audio, int direction) {
  if (!audio.decoderReady) return;
  int64_t deltaFrames = static_cast<int64_t>(direction) * 5 * audio.sampleRate;
  int64_t current = static_cast<int64_t>(audio.state.audioClockFrames.load());
  if (audio.state.seekRequested.load()) {
    current = audio.state.pendingSeekFrames.load();
  }
  requestAudioSeekFrame(audio, current + deltaFrames);
}

static void audioSeekToRatio(AudioPlaybackState& audio, double ratio) {
  if (!audio.decoderReady) return;
  uint64_t total = audio.state.totalFrames.load();
  if (total == 0) return;
  ratio = std::clamp(ratio, 0.0, 1.0);
  int64_t target = static_cast<int64_t>(ratio * static_cast<double>(total));
  requestAudioSeekFrame(audio, target);
}

static void audioCycleRadioFilter(AudioPlaybackState& audio) {
  audio.state.radioFilter.cycleMode();
  audio.state.radioDspCv.notify_all();
}

static void audioSetHold(AudioPlaybackState& audio, bool hold) {
  const bool wasHold = audio.state.hold.exchange(hold);
  if (wasHold && !hold) {
    audioPipelineTransitionRequestOutputFadeIn(audio.state.pipelineTransition,
                                               audio.state.sampleRate);
  }
}

static void audioAdjustVolume(AudioPlaybackState& audio, float delta) {
  float current = audio.state.volume.load(std::memory_order_relaxed);
  float next = std::clamp(current + delta, 0.0f, 4.0f);
  audio.state.volume.store(next, std::memory_order_relaxed);
}

static float audioGetVolume(const AudioPlaybackState& audio) {
  return audio.state.volume.load(std::memory_order_relaxed);
}

static float audioGetUnclippedOutputPeak(const AudioPlaybackState& audio) {
  return audio.state.unclippedOutputPeak.load(std::memory_order_relaxed);
}

static AudioMelodyInfo audioGetMelodyInfo(AudioPlaybackState& audio) {
  if (!audio.decoderReady) {
    return AudioMelodyInfo{};
  }
  MelodyOfflineAnalysisState analysisState = audio.melodyAnalysis.state();
  if (!analysisState.running && !analysisState.ready) {
    return {};
  }
  const MelodyOfflineFrame frame =
      audio.melodyAnalysis.frameAt(audioGetTimeSec(audio));
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

static AudioMelodyAnalysisState audioGetMelodyAnalysisState(
    const AudioPlaybackState& audio) {
  const MelodyOfflineAnalysisState state = audio.melodyAnalysis.state();
  AudioMelodyAnalysisState result;
  result.ready = state.ready;
  result.running = state.running;
  result.progress = state.progress;
  result.frameCount = state.frameCount;
  result.error = state.error;
  return result;
}

static bool audioAnalyzeFileToMelodyFile(
    const AudioPlaybackState& audio, const std::filesystem::path& file,
    int trackIndex, const std::filesystem::path& outputFile,
    const std::function<void(float)>& progressCallback,
    const std::function<bool()>& cancellationRequested, std::string* error,
    const std::function<bool()>& outputCommitStarted) {
  if (file.empty() || !std::filesystem::exists(file)) {
    if (error) *error = "Input file not found.";
    return false;
  }
  uint32_t analysisSampleRate = std::max<uint32_t>(1u, audio.sampleRate);
  uint32_t analysisChannels =
      std::clamp<uint32_t>(std::max<uint32_t>(1u, audio.baseChannels), 1u, 2u);
  return melodyOfflineAnalyzeToFile(
      file, trackIndex, analysisSampleRate, analysisChannels, 0,
      audio.kssOptions, audio.nsfOptions, audio.vgmOptions,
      audio.vgmDeviceOverrides, outputFile, progressCallback,
      cancellationRequested, error, outputCommitStarted);
}

static bool audioCanAnalyzeFileToMelodyFile(
    const std::filesystem::path& file) {
  return melodyOfflineCanAnalyzeFile(file);
}

static std::string audioGetWarning(const AudioPlaybackState& audio) {
  std::string warning = warningForBackend(audio, audio.state.backend);
  if (!warning.empty()) return warning;
  return audio.lastInitError;
}
