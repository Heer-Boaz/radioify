#pragma once

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "core/runtime_defaults.h"
#include "audition_worker.h"
#include "clock.h"
#include "ffmpegaudio.h"
#include "gmeaudio.h"
#include "gsfaudio.h"
#include "kssaudio.h"
#include "kssoptions.h"
#include "midiaudio.h"
#include "m4adecoder.h"
#include "miniaudio.h"
#include "nsfoptions.h"
#include "pipeline_transition.h"
#include "playback_backend.h"
#include "queued_audio_source.h"
#include "playback_source_buffer.h"
#include "output_volume_safety.h"
#include "psfaudio.h"
#include "radio.h"
#include "audiofilter/radio1938/preview/radio_preview_pipeline.h"
#include "vgmaudio.h"
#include "vgmoptions.h"

constexpr uint32_t kRadioProcessChannels = 1u;

struct AudioState {
  ma_decoder decoder{};
  FfmpegAudioDecoder ffmpeg{};
  GmeAudioDecoder gme{};
  GsfAudioDecoder gsf{};
  VgmAudioDecoder vgm{};
  KssAudioDecoder kss{};
  PsfAudioDecoder psf{};
  MidiAudioDecoder midi{};
  ma_device device{};
  Radio1938 radio1938{};
  PlaybackSourceBuffer m4aBuffer;
  std::mutex m4aMutex;
  std::condition_variable m4aCv;
  std::thread m4aThread;
  bool m4aInitDone = false;
  bool m4aInitOk = false;
  std::string m4aInitError;
  std::atomic<bool> m4aThreadRunning{false};
  std::atomic<bool> m4aStop{false};
  std::atomic<bool> sourcePreparing{false};
  std::atomic<bool> sourceAtEnd{false};
  std::thread decodeThread;
  std::atomic<bool> decodeThreadRunning{false};
  std::atomic<bool> decodeStop{false};
  std::mutex decodeMutex;
  std::condition_variable decodeCv;
  std::atomic<bool> externalStream{false};
  std::atomic<bool> paused{false};
  std::atomic<bool> hold{false};
  std::atomic<bool> finished{false};
  std::atomic<bool> useRadio1938{true};
  std::atomic<AudioMode> mode{AudioMode::None};
  const AudioBackendHandlers* backend = nullptr;
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
  std::atomic<float> peak{0.0f};
  std::atomic<int64_t> clipAlertUntilUs{0};
  std::atomic<bool> radioResetPending{false};
  OutputVolumeSafetyState outputSafety;
  AudioPipelineTransition pipelineTransition;
  AudioSampleRing streamRb;
  std::atomic<bool> streamQueueEnabled{false};
  std::atomic<int> streamSerial{0};
  std::atomic<int> pendingStreamSerial{0};
  std::atomic<int64_t> pendingStreamDiscardPtsUs{0};
  std::atomic<bool> streamSerialFlushPending{false};
  std::atomic<bool> streamBaseValid{false};
  std::atomic<int64_t> streamBasePtsUs{0};
  std::atomic<uint64_t> streamReadFrames{0};
  std::atomic<bool> streamClockReady{false};
  std::atomic<bool> deviceRecoveryPending{false};
  std::atomic<bool> deviceStopExpected{false};
  std::deque<AudioMetadata> streamMetadata;
  std::mutex streamMetadataMutex;
  std::condition_variable streamUpdateCv;
  std::atomic<uint64_t> streamUpdateCounter{0};
  std::atomic<bool> streamStarved{false};
  std::atomic<int64_t> streamDiscardPtsUs{0};
  std::atomic<uint64_t> deviceDelayFrames{0};
  Clock audioClock;
  uint32_t channels = 1;
  uint32_t sampleRate = 48000;
  bool dry = false;
  RadioAmIngressConfig radioAmIngress;
  RadioPreviewConfig radioPreviewConfig;
  RadioPreviewPipeline radioPreview;
};

struct AudioPlaybackState {
  AudioState state{};
  Radio1938 radio1938Template{};
  bool deviceReady = false;
  bool decoderReady = false;
  bool enableAudio = false;
  bool dry = false;
  bool enableRadio = kDefaultRadioFilterEnabled;
  uint32_t sampleRate = 48000;
  uint32_t baseChannels = 1;
  uint32_t channels = 1;
  float lpHz = 0.0f;
  float noise = 0.0f;
  std::string radioSettingsPath;
  std::string radioPresetName;
  std::filesystem::path nowPlaying;
  int trackIndex = 0;
  std::string lastInitError;
  std::string gmeWarning;
  std::string gsfWarning;
  std::string vgmWarning;
  KssPlaybackOptions kssOptions{};
  NsfPlaybackOptions nsfOptions{};
  VgmPlaybackOptions vgmOptions{};
  std::filesystem::path vgmDevicesFile;
  std::vector<VgmDeviceInfo> vgmDevices;
  std::unordered_map<uint32_t, VgmDeviceOptions> vgmDeviceDefaults;
  std::unordered_map<uint32_t, VgmDeviceOptions> vgmDeviceOverrides;
  AuditionState audition{};
};

extern AudioPlaybackState gAudio;

void appendAudioTimingLogLine(const char* line);
void drainPlaybackPipelineForReplacement(AudioState* state);
void rebuildRadioPreviewChain(AudioState* state);
void applyRadioTogglePreset();
void audioPlaybackHoldClipAlert(AudioState* state);
bool audioPlaybackFinishSeekPipelineTransition(AudioState* state,
                                               bool resetRadio = true);
void audioPlaybackProcessRadioBlock(AudioState* state,
                                    float* samples,
                                    uint32_t frames);
void dataCallback(ma_device* device, void* output, const void* input,
                  ma_uint32 frameCount);
void stopAndUninitActiveDecoder();
void stopPlayback();
bool loadFileAt(const std::filesystem::path& file, uint64_t startFrame,
               int trackIndex);
