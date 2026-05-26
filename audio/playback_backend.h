#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

struct VgmDeviceInfo;

enum class AudioMode : int {
  None = 0,
  Stream,
  M4a,
  Ffmpeg,
  Flac,
  Miniaudio,
  Gme,
  Midi,
  Gsf,
  Vgm,
  Kss,
  Psf,
};

using BackendInitProc = bool (*)(const std::filesystem::path& file,
                                uint64_t startFrame, int trackIndex,
                                std::string* error);
using BackendUninitProc = void (*)();
using BackendReadProc = bool (*)(float* out, uint32_t frameCount,
                                uint64_t* framesRead);
using BackendSeekProc = bool (*)(uint64_t frame);
using BackendTotalFramesProc = bool (*)(uint64_t* outFrames);
using BackendWarningProc = std::string (*)();

struct AudioBackendHandlers {
  AudioMode mode = AudioMode::None;
  bool supportsTrackIndex = false;
  bool allowSeekInCallback = true;
  bool finishOnShortRead = true;
  bool allowConcurrentOfflineAnalysis = true;
  BackendInitProc init = nullptr;
  BackendUninitProc uninit = nullptr;
  BackendReadProc read = nullptr;
  BackendSeekProc seek = nullptr;
  BackendTotalFramesProc totalFrames = nullptr;
  BackendWarningProc warning = nullptr;
};

AudioMode currentAudioMode();
bool isAudioMode(AudioMode mode);
const VgmDeviceInfo* findVgmDeviceInfo(uint32_t deviceId);

const AudioBackendHandlers* selectAudioBackend(
    const std::filesystem::path& file);
std::string warningForBackend(const AudioBackendHandlers* backend);
void activateBackend(const AudioBackendHandlers* backend, int trackIndex);
void storeTotalFramesFromBackend(const AudioBackendHandlers* backend);
bool openDecoderForBackend(const AudioBackendHandlers* backend,
                           const std::filesystem::path& file,
                           uint64_t startFrame,
                           int trackIndex);
void uninitOpenedDecoder(const AudioBackendHandlers* backend);
void seekLoadedDecoderToStart(const AudioBackendHandlers* backend,
                              uint64_t* startFrame);
