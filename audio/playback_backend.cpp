#include "playback_backend.h"

#include <atomic>

#include "audioplayback_internal.h"
#include "media_formats.h"
#include "miniaudio.h"
#include "miniaudio_file_path.h"
#include "playback_sources/m4a_playback_source.h"

namespace {

void applyVgmDeviceOverrides(AudioPlaybackState& audio) {
  if (audio.state.mode.load(std::memory_order_relaxed) != AudioMode::Vgm) {
    return;
  }
  if (audio.vgmDeviceOverrides.empty()) return;
  for (const auto& entry : audio.vgmDeviceOverrides) {
    audio.state.vgm.setDeviceOptions(entry.first, entry.second);
  }
}

bool initFfmpegBackend(AudioPlaybackState& audio,
                       const std::filesystem::path& file, uint64_t, int,
                       std::string* error) {
  audio.state.totalFrames.store(0);
  return audio.state.ffmpeg.init(file, audio.channels, audio.sampleRate, error);
}

void uninitFfmpegBackend(AudioPlaybackState& audio) {
  audio.state.ffmpeg.uninit();
}

bool readFfmpegBackend(AudioPlaybackState& audio, float* out,
                       uint32_t frameCount, uint64_t* framesRead) {
  return audio.state.ffmpeg.readFrames(out, frameCount, framesRead);
}

bool seekFfmpegBackend(AudioPlaybackState& audio, uint64_t frame) {
  return audio.state.ffmpeg.seekToFrame(frame);
}

bool totalFfmpegBackend(AudioPlaybackState& audio, uint64_t* outFrames) {
  if (!outFrames) return false;
  return audio.state.ffmpeg.getTotalFrames(outFrames);
}

bool initKssBackend(AudioPlaybackState& audio,
                    const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return audio.state.kss.init(file, audio.channels, audio.sampleRate, error,
                              trackIndex, audio.kssOptions);
}

void uninitKssBackend(AudioPlaybackState& audio) { audio.state.kss.uninit(); }

bool readKssBackend(AudioPlaybackState& audio, float* out, uint32_t frameCount,
                    uint64_t* framesRead) {
  return audio.state.kss.readFrames(out, frameCount, framesRead);
}

bool seekKssBackend(AudioPlaybackState& audio, uint64_t frame) {
  return audio.state.kss.seekToFrame(frame);
}

bool totalKssBackend(AudioPlaybackState& audio, uint64_t* outFrames) {
  return audio.state.kss.getTotalFrames(outFrames);
}

bool initPsfBackend(AudioPlaybackState& audio,
                    const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return audio.state.psf.init(file, audio.channels, audio.sampleRate, error,
                              trackIndex);
}

void uninitPsfBackend(AudioPlaybackState& audio) { audio.state.psf.uninit(); }

bool readPsfBackend(AudioPlaybackState& audio, float* out, uint32_t frameCount,
                    uint64_t* framesRead) {
  return audio.state.psf.readFrames(out, frameCount, framesRead);
}

bool seekPsfBackend(AudioPlaybackState& audio, uint64_t frame) {
  return audio.state.psf.seekToFrame(frame);
}

bool totalPsfBackend(AudioPlaybackState& audio, uint64_t* outFrames) {
  return audio.state.psf.getTotalFrames(outFrames);
}

bool initGsfBackend(AudioPlaybackState& audio,
                    const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  return audio.state.gsf.init(file, audio.channels, audio.sampleRate, error,
                              trackIndex);
}

void uninitGsfBackend(AudioPlaybackState& audio) { audio.state.gsf.uninit(); }

bool readGsfBackend(AudioPlaybackState& audio, float* out, uint32_t frameCount,
                    uint64_t* framesRead) {
  return audio.state.gsf.readFrames(out, frameCount, framesRead);
}

bool seekGsfBackend(AudioPlaybackState& audio, uint64_t frame) {
  return audio.state.gsf.seekToFrame(frame);
}

bool totalGsfBackend(AudioPlaybackState& audio, uint64_t* outFrames) {
  return audio.state.gsf.getTotalFrames(outFrames);
}

bool initVgmBackend(AudioPlaybackState& audio,
                    const std::filesystem::path& file, uint64_t, int,
                    std::string* error) {
  if (!audio.state.vgm.init(file, audio.channels, audio.sampleRate, error)) {
    return false;
  }
  audio.state.vgm.applyOptions(audio.vgmOptions);
  applyVgmDeviceOverrides(audio);
  audio.vgmWarning = audio.state.vgm.warning();
  return true;
}

void uninitVgmBackend(AudioPlaybackState& audio) { audio.state.vgm.uninit(); }

bool readVgmBackend(AudioPlaybackState& audio, float* out, uint32_t frameCount,
                    uint64_t* framesRead) {
  return audio.state.vgm.readFrames(out, frameCount, framesRead);
}

bool seekVgmBackend(AudioPlaybackState& audio, uint64_t frame) {
  return audio.state.vgm.seekToFrame(frame);
}

bool totalVgmBackend(AudioPlaybackState& audio, uint64_t* outFrames) {
  return audio.state.vgm.getTotalFrames(outFrames);
}

bool initGmeBackend(AudioPlaybackState& audio,
                    const std::filesystem::path& file, uint64_t, int trackIndex,
                    std::string* error) {
  if (!audio.state.gme.init(file, audio.channels, audio.sampleRate, error,
                            trackIndex)) {
    return false;
  }
  audio.state.gme.applyNsfOptions(audio.nsfOptions);
  audio.gmeWarning = audio.state.gme.warning();
  return true;
}

void uninitGmeBackend(AudioPlaybackState& audio) { audio.state.gme.uninit(); }

bool readGmeBackend(AudioPlaybackState& audio, float* out, uint32_t frameCount,
                    uint64_t* framesRead) {
  return audio.state.gme.readFrames(out, frameCount, framesRead);
}

bool seekGmeBackend(AudioPlaybackState& audio, uint64_t frame) {
  return audio.state.gme.seekToFrame(frame);
}

bool totalGmeBackend(AudioPlaybackState& audio, uint64_t* outFrames) {
  return audio.state.gme.getTotalFrames(outFrames);
}

bool initMidiBackend(AudioPlaybackState& audio,
                     const std::filesystem::path& file, uint64_t, int,
                     std::string* error) {
  return audio.state.midi.init(file, audio.channels, audio.sampleRate, error);
}

void uninitMidiBackend(AudioPlaybackState& audio) { audio.state.midi.uninit(); }

bool readMidiBackend(AudioPlaybackState& audio, float* out, uint32_t frameCount,
                     uint64_t* framesRead) {
  return audio.state.midi.readFrames(out, frameCount, framesRead);
}

bool seekMidiBackend(AudioPlaybackState& audio, uint64_t frame) {
  return audio.state.midi.seekToFrame(frame);
}

bool totalMidiBackend(AudioPlaybackState& audio, uint64_t* outFrames) {
  return audio.state.midi.getTotalFrames(outFrames);
}

bool initMiniaudioBackend(AudioPlaybackState& audio,
                          const std::filesystem::path& file, uint64_t, int,
                          std::string*) {
  ma_decoder_config decConfig =
      ma_decoder_config_init(ma_format_f32, audio.channels, audio.sampleRate);
  return maDecoderInitFilePath(file, &decConfig, &audio.state.decoder) ==
         MA_SUCCESS;
}

void uninitMiniaudioBackend(AudioPlaybackState& audio) {
  ma_decoder_uninit(&audio.state.decoder);
}

bool readMiniaudioBackend(AudioPlaybackState& audio, float* out,
                          uint32_t frameCount, uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  ma_uint64 read = 0;
  ma_result result =
      ma_decoder_read_pcm_frames(&audio.state.decoder, out, frameCount, &read);
  if (framesRead) *framesRead = static_cast<uint64_t>(read);
  return result == MA_SUCCESS || result == MA_AT_END;
}

bool seekMiniaudioBackend(AudioPlaybackState& audio, uint64_t frame) {
  return ma_decoder_seek_to_pcm_frame(
             &audio.state.decoder, static_cast<ma_uint64>(frame)) == MA_SUCCESS;
}

bool totalMiniaudioBackend(AudioPlaybackState& audio, uint64_t* outFrames) {
  if (!outFrames) return false;
  ma_uint64 total = 0;
  if (ma_decoder_get_length_in_pcm_frames(&audio.state.decoder, &total) !=
      MA_SUCCESS) {
    return false;
  }
  *outFrames = static_cast<uint64_t>(total);
  return true;
}

std::string warningGmeBackend(const AudioPlaybackState& audio) {
  return audio.gmeWarning;
}
std::string warningGsfBackend(const AudioPlaybackState& audio) {
  return audio.gsfWarning;
}
std::string warningVgmBackend(const AudioPlaybackState& audio) {
  return audio.vgmWarning;
}

const AudioBackendHandlers kBackendM4a{
    AudioMode::M4a, false, false, true, initM4aBackend,
    uninitM4aBackend, nullptr, nullptr, totalM4aBackend, nullptr};
const AudioBackendHandlers kBackendFfmpeg{
    AudioMode::Ffmpeg, false, true, true, initFfmpegBackend,
    uninitFfmpegBackend, readFfmpegBackend, seekFfmpegBackend,
    totalFfmpegBackend, nullptr};
const AudioBackendHandlers kBackendKss{
    AudioMode::Kss, true, true, true, initKssBackend,
    uninitKssBackend, readKssBackend, seekKssBackend, totalKssBackend,
    nullptr};
const AudioBackendHandlers kBackendPsf{
    AudioMode::Psf, true, true, false, initPsfBackend,
    uninitPsfBackend, readPsfBackend, seekPsfBackend, totalPsfBackend,
    nullptr};
const AudioBackendHandlers kBackendGsf{
    AudioMode::Gsf, true, true, false, initGsfBackend,
    uninitGsfBackend, readGsfBackend, seekGsfBackend, totalGsfBackend,
    warningGsfBackend};
const AudioBackendHandlers kBackendVgm{
    AudioMode::Vgm, true, true, true, initVgmBackend,
    uninitVgmBackend, readVgmBackend, seekVgmBackend, totalVgmBackend,
    warningVgmBackend};
const AudioBackendHandlers kBackendGme{
    AudioMode::Gme, true, true, true, initGmeBackend,
    uninitGmeBackend, readGmeBackend, seekGmeBackend, totalGmeBackend,
    warningGmeBackend};
const AudioBackendHandlers kBackendMidi{
    AudioMode::Midi, false, true, true, initMidiBackend,
    uninitMidiBackend, readMidiBackend, seekMidiBackend, totalMidiBackend,
    nullptr};
const AudioBackendHandlers kBackendMiniaudio{
    AudioMode::Miniaudio, false, true, true, initMiniaudioBackend,
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

AudioMode currentAudioMode(const AudioPlaybackState& audio) {
  return audio.state.mode.load(std::memory_order_relaxed);
}

bool isAudioMode(const AudioPlaybackState& audio, AudioMode mode) {
  return currentAudioMode(audio) == mode;
}

const AudioBackendHandlers* selectAudioBackend(
    const std::filesystem::path& file) {
  for (const auto& entry : kBackends) {
    if (entry.matches(file)) return entry.backend;
  }
  return nullptr;
}

std::string warningForBackend(const AudioPlaybackState& audio,
                              const AudioBackendHandlers* backend) {
  if (backend && backend->warning) {
    return backend->warning(audio);
  }
  return {};
}

void activateBackend(AudioPlaybackState& audio,
                     const AudioBackendHandlers* backend, int trackIndex) {
  audio.decoderReady = true;
  audio.state.backend = backend;
  audio.state.mode.store(backend ? backend->mode : AudioMode::None,
                         std::memory_order_relaxed);
  audio.state.externalStream.store(false);
  audio.trackIndex = (backend && backend->supportsTrackIndex) ? trackIndex : 0;
}

void storeTotalFramesFromBackend(AudioPlaybackState& audio,
                                 const AudioBackendHandlers* backend) {
  if (!backend || !backend->totalFrames) {
    return;
  }
  uint64_t total = 0;
  bool ok = backend->totalFrames(audio, &total);
  audio.state.totalFrames.store(ok ? total : 0);
}

bool openDecoderForBackend(AudioPlaybackState& audio,
                           const AudioBackendHandlers* backend,
                           const std::filesystem::path& file,
                           uint64_t startFrame, int trackIndex) {
  if (!backend || !backend->init) return false;
  std::string error;
  if (!backend->init(audio, file, startFrame, trackIndex, &error)) {
    audio.lastInitError = error;
    if (backend->mode == AudioMode::Gsf) {
      audio.gsfWarning = error;
    } else if (backend->mode == AudioMode::Vgm) {
      audio.vgmWarning = error;
    } else {
      audio.gmeWarning = error;
    }
    return false;
  }
  audio.lastInitError.clear();
  storeTotalFramesFromBackend(audio, backend);
  return true;
}

void uninitOpenedDecoder(AudioPlaybackState& audio,
                         const AudioBackendHandlers* backend) {
  if (backend && backend->uninit) {
    backend->uninit(audio);
  }
}

void seekLoadedDecoderToStart(AudioPlaybackState& audio,
                              const AudioBackendHandlers* backend,
                              uint64_t* startFrame) {
  if (!backend || !startFrame || !backend->seek) return;
  uint64_t total = audio.state.totalFrames.load();
  if (total > 0 && *startFrame > total) {
    *startFrame = total;
  }
  if (*startFrame == 0) return;
  if (!backend->seek(audio, *startFrame)) {
    *startFrame = 0;
    backend->seek(audio, 0);
  }
}
