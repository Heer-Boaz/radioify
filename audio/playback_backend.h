#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

enum class AudioMode : int {
  None = 0,
  Stream,
  M4a,
  Ffmpeg,
  Miniaudio,
  Gme,
  Midi,
  Gsf,
  Vgm,
  Kss,
  Psf,
};

struct AudioPlaybackState;

using BackendInitProc = bool (*)(AudioPlaybackState& audio,
                                 const std::filesystem::path& file,
                                 uint64_t startFrame, int trackIndex,
                                 std::string* error);
using BackendUninitProc = void (*)(AudioPlaybackState& audio);
using BackendReadProc = bool (*)(AudioPlaybackState& audio, float* out,
                                 uint32_t frameCount, uint64_t* framesRead);
using BackendSeekProc = bool (*)(AudioPlaybackState& audio, uint64_t frame);
using BackendTotalFramesProc = bool (*)(AudioPlaybackState& audio,
                                        uint64_t* outFrames);
using BackendWarningProc = std::string (*)(const AudioPlaybackState& audio);

struct AudioBackendHandlers {
  AudioMode mode = AudioMode::None;
  bool supportsTrackIndex = false;
  bool finishOnShortRead = true;
  bool allowConcurrentOfflineAnalysis = true;
  BackendInitProc init = nullptr;
  BackendUninitProc uninit = nullptr;
  BackendReadProc read = nullptr;
  BackendSeekProc seek = nullptr;
  BackendTotalFramesProc totalFrames = nullptr;
  BackendWarningProc warning = nullptr;
};

AudioMode currentAudioMode(const AudioPlaybackState& audio);
bool isAudioMode(const AudioPlaybackState& audio, AudioMode mode);

const AudioBackendHandlers* selectAudioBackend(
    const std::filesystem::path& file);
std::string warningForBackend(const AudioPlaybackState& audio,
                              const AudioBackendHandlers* backend);
void activateBackend(AudioPlaybackState& audio,
                     const AudioBackendHandlers* backend, int trackIndex);
void storeTotalFramesFromBackend(AudioPlaybackState& audio,
                                 const AudioBackendHandlers* backend);
bool openDecoderForBackend(AudioPlaybackState& audio,
                           const AudioBackendHandlers* backend,
                           const std::filesystem::path& file,
                           uint64_t startFrame, int trackIndex);
void uninitOpenedDecoder(AudioPlaybackState& audio,
                         const AudioBackendHandlers* backend);
void seekLoadedDecoderToStart(AudioPlaybackState& audio,
                              const AudioBackendHandlers* backend,
                              uint64_t* startFrame);
