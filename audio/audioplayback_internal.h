#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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
#include "miniaudio.h"
#include "nsfoptions.h"
#include "pipeline_transition.h"
#include "playback_backend.h"
#include "output_volume_safety.h"
#include "psfaudio.h"
#include "queued_audio_source.h"
#include "radio_playback.h"
#include "spsc_audio_ring.h"
#include "spsc_audio_timeline.h"
#include "vgmaudio.h"
#include "vgmoptions.h"

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
  RadioPlaybackFilter radioFilter;
  std::thread m4aThread;
  bool m4aInitDone = false;
  bool m4aInitOk = false;
  std::string m4aInitError;
  std::atomic<bool> m4aThreadRunning{false};
  std::atomic<bool> m4aStop{false};
  std::atomic<bool> sourcePreparing{false};
  std::atomic<bool> sourceAtEnd{false};
  std::atomic<bool> processedAtEnd{false};
  std::thread decodeThread;
  std::atomic<bool> decodeThreadRunning{false};
  std::atomic<bool> decodeStop{false};
  std::mutex audioQueueMutex;
  std::condition_variable audioQueueCv;
  std::condition_variable radioDspCv;
  std::atomic<bool> externalStream{false};
  std::atomic<bool> paused{false};
  std::atomic<bool> hold{false};
  std::atomic<bool> finished{false};
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
  OutputVolumeSafetyState outputSafety;
  AudioPipelineTransition pipelineTransition;
  SpscAudioRing decodedAudio;
  SpscAudioRing processedAudio;
  std::thread radioDspThread;
  std::atomic<bool> radioDspThreadRunning{false};
  std::atomic<bool> radioDspStop{false};
  std::atomic<uint64_t> sourceResetRequestGeneration{0};
  std::atomic<uint64_t> sourceResetAppliedGeneration{0};
  std::atomic<uint64_t> sourceResetFramePosition{0};
  std::atomic<bool> streamQueueEnabled{false};
  std::atomic<int> streamSerial{0};
  std::mutex streamResetMutex;
  AudioStreamResetRequest pendingStreamReset;
  std::atomic<uint64_t> streamResetRequestedGeneration{0};
  std::atomic<uint64_t> streamResetAppliedGeneration{0};
  std::atomic<bool> streamClockReady{false};
  std::atomic<bool> deviceRecoveryPending{false};
  std::atomic<bool> deviceStopExpected{false};
  SpscAudioTimeline decodedTimeline;
  SpscAudioTimeline processedTimeline;
  SpscAudioEventTimeline processedEvents;
  std::mutex streamUpdateMutex;
  std::condition_variable streamUpdateCv;
  std::atomic<uint64_t> streamUpdateCounter{0};
  std::atomic<bool> streamStarved{false};
  std::atomic<int64_t> streamDiscardPtsUs{0};
  std::atomic<uint64_t> deviceDelayFrames{0};
  Clock audioClock;
  uint32_t channels = 1;
  uint32_t sampleRate = 48000;
  bool dry = false;
};

struct AudioPlaybackState {
  AudioState state{};
  bool deviceReady = false;
  bool decoderReady = false;
  bool enableAudio = false;
  uint32_t sampleRate = 48000;
  uint32_t baseChannels = 1;
  uint32_t channels = 1;
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
bool audioPlaybackFinishSeekPipelineTransition(AudioState* state);
void dataCallback(ma_device* device, void* output, const void* input,
                  ma_uint32 frameCount);
void stopAndUninitActiveDecoder();
void stopPlayback();
bool loadFileAt(const std::filesystem::path& file, uint64_t startFrame,
               int trackIndex);
