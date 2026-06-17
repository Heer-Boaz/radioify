#include "playback_backend.h"

#include <atomic>

#include "audioplayback_internal.h"
#include "media_formats.h"
#include "miniaudio.h"
#include "miniaudio_file_path.h"
#include "playback_sources/m4a_playback_source.h"

namespace {

void applyVgmDeviceOverrides() {
  if (gAudio.state.mode.load(std::memory_order_relaxed) != AudioMode::Vgm) {
    return;
  }
  if (gAudio.vgmDeviceOverrides.empty()) return;
  for (const auto& entry : gAudio.vgmDeviceOverrides) {
    gAudio.state.vgm.setDeviceOptions(entry.first, entry.second);
  }
}

bool initFfmpegBackend(const std::filesystem::path& file, uint64_t, int,
                       std::string* error) {
  gAudio.state.totalFrames.store(0);
  return gAudio.state.ffmpeg.init(file, gAudio.channels, gAudio.sampleRate,
                                  error);
}

void uninitFfmpegBackend() { gAudio.state.ffmpeg.uninit(); }

bool readFfmpegBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.ffmpeg.readFrames(out, frameCount, framesRead);
}

bool seekFfmpegBackend(uint64_t frame) {
  return gAudio.state.ffmpeg.seekToFrame(frame);
}

bool totalFfmpegBackend(uint64_t* outFrames) {
  if (!outFrames) return false;
  return gAudio.state.ffmpeg.getTotalFrames(outFrames);
}

bool initKssBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return gAudio.state.kss.init(file, gAudio.channels, gAudio.sampleRate, error,
                               trackIndex, gAudio.kssOptions);
}

void uninitKssBackend() { gAudio.state.kss.uninit(); }

bool readKssBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.kss.readFrames(out, frameCount, framesRead);
}

bool seekKssBackend(uint64_t frame) {
  return gAudio.state.kss.seekToFrame(frame);
}

bool totalKssBackend(uint64_t* outFrames) {
  return gAudio.state.kss.getTotalFrames(outFrames);
}

bool initPsfBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return gAudio.state.psf.init(file, gAudio.channels, gAudio.sampleRate, error,
                               trackIndex);
}

void uninitPsfBackend() { gAudio.state.psf.uninit(); }

bool readPsfBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.psf.readFrames(out, frameCount, framesRead);
}

bool seekPsfBackend(uint64_t frame) {
  return gAudio.state.psf.seekToFrame(frame);
}

bool totalPsfBackend(uint64_t* outFrames) {
  return gAudio.state.psf.getTotalFrames(outFrames);
}

bool initGsfBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return gAudio.state.gsf.init(file, gAudio.channels, gAudio.sampleRate, error,
                               trackIndex);
}

void uninitGsfBackend() { gAudio.state.gsf.uninit(); }

bool readGsfBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.gsf.readFrames(out, frameCount, framesRead);
}

bool seekGsfBackend(uint64_t frame) {
  return gAudio.state.gsf.seekToFrame(frame);
}

bool totalGsfBackend(uint64_t* outFrames) {
  return gAudio.state.gsf.getTotalFrames(outFrames);
}

bool initVgmBackend(const std::filesystem::path& file, uint64_t, int,
                    std::string* error) {
  if (!gAudio.state.vgm.init(file, gAudio.channels, gAudio.sampleRate, error)) {
    return false;
  }
  gAudio.state.vgm.applyOptions(gAudio.vgmOptions);
  applyVgmDeviceOverrides();
  gAudio.vgmWarning = gAudio.state.vgm.warning();
  return true;
}

void uninitVgmBackend() { gAudio.state.vgm.uninit(); }

bool readVgmBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.vgm.readFrames(out, frameCount, framesRead);
}

bool seekVgmBackend(uint64_t frame) {
  return gAudio.state.vgm.seekToFrame(frame);
}

bool totalVgmBackend(uint64_t* outFrames) {
  return gAudio.state.vgm.getTotalFrames(outFrames);
}

bool initGmeBackend(const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  if (!gAudio.state.gme.init(file, gAudio.channels, gAudio.sampleRate, error,
                             trackIndex)) {
    return false;
  }
  gAudio.state.gme.applyNsfOptions(gAudio.nsfOptions);
  gAudio.gmeWarning = gAudio.state.gme.warning();
  return true;
}

void uninitGmeBackend() { gAudio.state.gme.uninit(); }

bool readGmeBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.gme.readFrames(out, frameCount, framesRead);
}

bool seekGmeBackend(uint64_t frame) {
  return gAudio.state.gme.seekToFrame(frame);
}

bool totalGmeBackend(uint64_t* outFrames) {
  return gAudio.state.gme.getTotalFrames(outFrames);
}

bool initMidiBackend(const std::filesystem::path& file, uint64_t, int,
                     std::string* error) {
  return gAudio.state.midi.init(file, gAudio.channels, gAudio.sampleRate,
                                error);
}

void uninitMidiBackend() { gAudio.state.midi.uninit(); }

bool readMidiBackend(float* out, uint32_t frameCount, uint64_t* framesRead) {
  return gAudio.state.midi.readFrames(out, frameCount, framesRead);
}

bool seekMidiBackend(uint64_t frame) {
  return gAudio.state.midi.seekToFrame(frame);
}

bool totalMidiBackend(uint64_t* outFrames) {
  return gAudio.state.midi.getTotalFrames(outFrames);
}

bool initMiniaudioBackend(const std::filesystem::path& file, uint64_t, int,
                          std::string*) {
  ma_decoder_config decConfig =
      ma_decoder_config_init(ma_format_f32, gAudio.channels, gAudio.sampleRate);
  return maDecoderInitFilePath(file, &decConfig, &gAudio.state.decoder) ==
         MA_SUCCESS;
}

void uninitMiniaudioBackend() { ma_decoder_uninit(&gAudio.state.decoder); }

bool readMiniaudioBackend(float* out, uint32_t frameCount,
                          uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  ma_uint64 read = 0;
  ma_result result =
      ma_decoder_read_pcm_frames(&gAudio.state.decoder, out, frameCount, &read);
  if (framesRead) *framesRead = static_cast<uint64_t>(read);
  return result == MA_SUCCESS || result == MA_AT_END;
}

bool seekMiniaudioBackend(uint64_t frame) {
  return ma_decoder_seek_to_pcm_frame(&gAudio.state.decoder,
                                      static_cast<ma_uint64>(frame)) ==
         MA_SUCCESS;
}

bool totalMiniaudioBackend(uint64_t* outFrames) {
  if (!outFrames) return false;
  ma_uint64 total = 0;
  if (ma_decoder_get_length_in_pcm_frames(&gAudio.state.decoder, &total) !=
      MA_SUCCESS) {
    return false;
  }
  *outFrames = static_cast<uint64_t>(total);
  return true;
}

std::string warningGmeBackend() { return gAudio.gmeWarning; }
std::string warningGsfBackend() { return gAudio.gsfWarning; }
std::string warningVgmBackend() { return gAudio.vgmWarning; }

const AudioBackendHandlers kBackendM4a{
    AudioMode::M4a, false, false, false, true, initM4aBackend,
    uninitM4aBackend, readM4aBackend, nullptr, totalM4aBackend, nullptr};
const AudioBackendHandlers kBackendFfmpeg{
    AudioMode::Ffmpeg, false, true, true, true, initFfmpegBackend,
    uninitFfmpegBackend, readFfmpegBackend, seekFfmpegBackend,
    totalFfmpegBackend, nullptr};
const AudioBackendHandlers kBackendKss{
    AudioMode::Kss, true, true, true, true, initKssBackend,
    uninitKssBackend, readKssBackend, seekKssBackend, totalKssBackend,
    nullptr};
const AudioBackendHandlers kBackendPsf{
    AudioMode::Psf, true, true, true, false, initPsfBackend,
    uninitPsfBackend, readPsfBackend, seekPsfBackend, totalPsfBackend,
    nullptr};
const AudioBackendHandlers kBackendGsf{
    AudioMode::Gsf, true, true, true, false, initGsfBackend,
    uninitGsfBackend, readGsfBackend, seekGsfBackend, totalGsfBackend,
    warningGsfBackend};
const AudioBackendHandlers kBackendVgm{
    AudioMode::Vgm, true, true, true, true, initVgmBackend,
    uninitVgmBackend, readVgmBackend, seekVgmBackend, totalVgmBackend,
    warningVgmBackend};
const AudioBackendHandlers kBackendGme{
    AudioMode::Gme, true, true, true, true, initGmeBackend,
    uninitGmeBackend, readGmeBackend, seekGmeBackend, totalGmeBackend,
    warningGmeBackend};
const AudioBackendHandlers kBackendMidi{
    AudioMode::Midi, false, true, true, true, initMidiBackend,
    uninitMidiBackend, readMidiBackend, seekMidiBackend, totalMidiBackend,
    nullptr};
const AudioBackendHandlers kBackendMiniaudio{
    AudioMode::Miniaudio, false, true, true, true, initMiniaudioBackend,
    uninitMiniaudioBackend, readMiniaudioBackend, seekMiniaudioBackend,
    totalMiniaudioBackend, nullptr};

struct BackendSelector {
  bool (*matches)(const std::filesystem::path& file);
  const AudioBackendHandlers* backend;
};

const BackendSelector kBackends[] = {
    {isM4aExt, &kBackendM4a},            {isFfmpegAudioExt, &kBackendFfmpeg},
    {isMiniaudioExt, &kBackendMiniaudio},
    {isGmeExt, &kBackendGme},            {isMidiExt, &kBackendMidi},
    {isGsfExt, &kBackendGsf},            {isVgmExt, &kBackendVgm},
    {isKssExt, &kBackendKss},            {isPsfExt, &kBackendPsf},
};

}  // namespace

AudioMode currentAudioMode() {
  return gAudio.state.mode.load(std::memory_order_relaxed);
}

bool isAudioMode(AudioMode mode) {
  return currentAudioMode() == mode;
}

const VgmDeviceInfo* findVgmDeviceInfo(uint32_t deviceId) {
  for (const auto& device : gAudio.vgmDevices) {
    if (device.id == deviceId) return &device;
  }
  return nullptr;
}

const AudioBackendHandlers* selectAudioBackend(
    const std::filesystem::path& file) {
  for (const auto& entry : kBackends) {
    if (entry.matches(file)) return entry.backend;
  }
  return nullptr;
}

std::string warningForBackend(const AudioBackendHandlers* backend) {
  if (backend && backend->warning) {
    return backend->warning();
  }
  return {};
}

void activateBackend(const AudioBackendHandlers* backend, int trackIndex) {
  gAudio.decoderReady = true;
  gAudio.state.backend = backend;
  gAudio.state.mode.store(backend ? backend->mode : AudioMode::None,
                          std::memory_order_relaxed);
  gAudio.state.externalStream.store(false);
  gAudio.trackIndex = (backend && backend->supportsTrackIndex) ? trackIndex : 0;
}

void storeTotalFramesFromBackend(const AudioBackendHandlers* backend) {
  if (!backend || !backend->totalFrames) {
    return;
  }
  uint64_t total = 0;
  bool ok = backend->totalFrames(&total);
  gAudio.state.totalFrames.store(ok ? total : 0);
}

bool openDecoderForBackend(const AudioBackendHandlers* backend,
                           const std::filesystem::path& file,
                           uint64_t startFrame,
                           int trackIndex) {
  if (!backend || !backend->init) return false;
  std::string error;
  if (!backend->init(file, startFrame, trackIndex, &error)) {
    gAudio.lastInitError = error;
    if (backend->mode == AudioMode::Gsf) {
      gAudio.gsfWarning = error;
    } else if (backend->mode == AudioMode::Vgm) {
      gAudio.vgmWarning = error;
    } else {
      gAudio.gmeWarning = error;
    }
    return false;
  }
  gAudio.lastInitError.clear();
  storeTotalFramesFromBackend(backend);
  return true;
}

void uninitOpenedDecoder(const AudioBackendHandlers* backend) {
  if (backend && backend->uninit) {
    backend->uninit();
  }
}

void seekLoadedDecoderToStart(const AudioBackendHandlers* backend,
                              uint64_t* startFrame) {
  if (!backend || !startFrame || !backend->seek) return;
  uint64_t total = gAudio.state.totalFrames.load();
  if (total > 0 && *startFrame > total) {
    *startFrame = total;
  }
  if (*startFrame == 0) return;
  if (!backend->seek(*startFrame)) {
    *startFrame = 0;
    backend->seek(0);
  }
}
