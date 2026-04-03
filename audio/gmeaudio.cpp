#include "gmeaudio.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr float kInt16ToFloat = 1.0f / 32768.0f;
constexpr int kDefaultTrackLengthMs = 150000;
constexpr double kNsfEqNesTreble = -1.0;
constexpr double kNsfEqFamicomTreble = -15.0;
constexpr double kNsfEqBass = 80.0;

void setError(std::string* error, const char* message) {
  if (error && message) {
    *error = message;
  }
}

void appendWarning(std::string* warning, const char* message) {
  if (!warning || !message || message[0] == '\0') return;
  if (!warning->empty()) {
    warning->append(" ");
  }
  warning->append(message);
}

uint64_t msToFrames(int64_t ms, uint32_t sampleRate) {
  if (ms <= 0 || sampleRate == 0) return 0;
  double frames = static_cast<double>(ms) * static_cast<double>(sampleRate) /
                  1000.0;
  return static_cast<uint64_t>(std::llround(frames));
}

int framesToMs(uint64_t frames, uint32_t sampleRate) {
  if (frames == 0 || sampleRate == 0) return 0;
  double ms = static_cast<double>(frames) * 1000.0 /
              static_cast<double>(sampleRate);
  if (ms > static_cast<double>(std::numeric_limits<int>::max())) {
    return std::numeric_limits<int>::max();
  }
  return static_cast<int>(std::llround(ms));
}

int computeTrackLengthMs(const gme_info_t* info) {
  if (!info) return kDefaultTrackLengthMs;
  if (info->length > 0) return info->length;
  int intro = info->intro_length > 0 ? info->intro_length : 0;
  int loop = info->loop_length > 0 ? info->loop_length : 0;
  if (intro > 0 || loop > 0) {
    return intro + loop * 2;
  }
  return kDefaultTrackLengthMs;
}

bool tryLoadM3u(Music_Emu* emu, const std::filesystem::path& path) {
  std::filesystem::path m3uPath = path;
  m3uPath.replace_extension(".m3u");
  if (!std::filesystem::exists(m3uPath)) {
    return false;
  }
  gme_load_m3u(emu, m3uPath.string().c_str());
  return true;
}

bool isNsfType(Music_Emu* emu) {
  if (!emu) return false;
  gme_type_t type = gme_type(emu);
  return type == gme_nsf_type || type == gme_nsfe_type;
}

double stereoDepthValue(NsfStereoDepth depth) {
  switch (depth) {
    case NsfStereoDepth::Low:
      return 0.5;
    case NsfStereoDepth::High:
      return 1.0;
    case NsfStereoDepth::Off:
    default:
      return 0.0;
  }
}

double tempoValue(NsfTempoMode mode) {
  return mode == NsfTempoMode::Pal50 ? (50.0 / 60.0) : 1.0;
}
}  // namespace

GmeAudioDecoder::GmeAudioDecoder() = default;
GmeAudioDecoder::~GmeAudioDecoder() { uninit(); }

bool GmeAudioDecoder::init(const std::filesystem::path& path,
                           uint32_t channels, uint32_t sampleRate,
                           std::string* error, int trackIndex) {
  uninit();

  if (channels != 1 && channels != 2) {
    setError(error, "Unsupported channel count for GME.");
    return false;
  }

  Music_Emu* emu = nullptr;
  gme_err_t openErr =
      gme_open_file(path.string().c_str(), &emu, static_cast<int>(sampleRate));
  if (openErr != nullptr) {
    setError(error, openErr);
    return false;
  }

  std::string warning;
  appendWarning(&warning, gme_warning(emu));
  gme_type_t type = gme_type(emu);
  isNsf_ = (type == gme_nsf_type || type == gme_nsfe_type);
  if (type == gme_kss_type) {
    gme_enable_accuracy(emu, 1);
  }

  tryLoadM3u(emu, path);
  int tracks = gme_track_count(emu);
  if (tracks <= 0) {
    gme_delete(emu);
    setError(error, "No tracks found in GME file.");
    return false;
  }
  if (trackIndex < 0) trackIndex = 0;
  if (trackIndex >= tracks) trackIndex = tracks - 1;

  gme_err_t startErr = gme_start_track(emu, trackIndex);
  if (startErr != nullptr) {
    gme_delete(emu);
    setError(error, startErr);
    return false;
  }
  appendWarning(&warning, gme_warning(emu));

  uint64_t totalFrames = 0;
  int baseLengthMs = 0;
  gme_info_t* info = nullptr;
  gme_err_t infoErr = gme_track_info(emu, &info, trackIndex);
  if (infoErr == nullptr && info) {
    int lengthMs = computeTrackLengthMs(info);
    baseLengthMs = lengthMs;
    gme_set_fade(emu, lengthMs);
    totalFrames = msToFrames(lengthMs, sampleRate);
    gme_free_info(info);
  }

  emu_ = emu;
  channels_ = channels;
  sampleRate_ = sampleRate;
  totalFrames_ = totalFrames;
  baseLengthMs_ = baseLengthMs;
  framePos_ = 0;
  atEnd_ = false;
  buffer_.clear();
  warning_ = std::move(warning);
  return true;
}

void GmeAudioDecoder::applyNsfOptions(const NsfPlaybackOptions& options) {
  if (!emu_) return;

  gme_equalizer_t eq{};
  eq.bass = kNsfEqBass;
  eq.treble = (options.eqPreset == NsfEqPreset::Famicom)
                  ? kNsfEqFamicomTreble
                  : kNsfEqNesTreble;
  gme_set_equalizer(emu_, &eq);
  gme_set_stereo_depth(emu_, stereoDepthValue(options.stereoDepth));
  gme_ignore_silence(emu_, options.ignoreSilence ? 1 : 0);
  double tempo = tempoValue(options.tempoMode);
  gme_set_tempo(emu_, tempo);
  if (baseLengthMs_ > 0) {
    int adjustedMs = static_cast<int>(
        std::llround(static_cast<double>(baseLengthMs_) / tempo));
    gme_set_fade(emu_, adjustedMs);
    totalFrames_ = msToFrames(adjustedMs, sampleRate_);
  }
}

bool gmeListTracks(const std::filesystem::path& path,
                   std::vector<TrackEntry>* out,
                   std::string* error) {
  if (!out) return false;
  out->clear();

  Music_Emu* emu = nullptr;
  gme_err_t openErr = gme_open_file(path.string().c_str(), &emu, gme_info_only);
  if (openErr != nullptr) {
    setError(error, openErr);
    return false;
  }

  tryLoadM3u(emu, path);
  int tracks = gme_track_count(emu);
  if (tracks <= 0) {
    gme_delete(emu);
    setError(error, "No tracks found in GME file.");
    return false;
  }

  out->reserve(static_cast<size_t>(tracks));
  for (int track = 0; track < tracks; ++track) {
    TrackEntry entry{};
    entry.index = track;
    entry.lengthMs = -1;

    gme_info_t* info = nullptr;
    gme_err_t infoErr = gme_track_info(emu, &info, track);
    if (infoErr == nullptr && info) {
      if (info->song && info->song[0]) {
        entry.title = info->song;
      }
      entry.lengthMs = computeTrackLengthMs(info);
      gme_free_info(info);
    }
    out->push_back(std::move(entry));
  }

  gme_delete(emu);
  return true;
}

void GmeAudioDecoder::uninit() {
  if (!emu_) return;
  gme_delete(emu_);
  emu_ = nullptr;
  channels_ = 0;
  sampleRate_ = 0;
  totalFrames_ = 0;
  baseLengthMs_ = 0;
  framePos_ = 0;
  atEnd_ = false;
  isNsf_ = false;
  buffer_.clear();
  warning_.clear();
}

bool GmeAudioDecoder::readFrames(float* out, uint32_t frameCount,
                                 uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!emu_ || !out || frameCount == 0) return false;

  if (atEnd_) {
    return true;
  }

  uint64_t toRead = frameCount;
  if (totalFrames_ > 0) {
    if (framePos_ >= totalFrames_) {
      atEnd_ = true;
      return true;
    }
    uint64_t remaining = totalFrames_ - framePos_;
    toRead = std::min<uint64_t>(toRead, remaining);
  }

  if (toRead == 0) {
    atEnd_ = true;
    return true;
  }

  static_assert(sizeof(int16_t) == sizeof(short),
                "GME expects 16-bit samples.");
  buffer_.resize(static_cast<size_t>(toRead) * 2u);
  gme_err_t playErr =
      gme_play(emu_, static_cast<int>(toRead * 2u),
               reinterpret_cast<short*>(buffer_.data()));
  if (playErr != nullptr) {
    return false;
  }

  if (channels_ == 2) {
    size_t samples = static_cast<size_t>(toRead) * 2u;
    for (size_t i = 0; i < samples; ++i) {
      out[i] = static_cast<float>(buffer_[i]) * kInt16ToFloat;
    }
  } else {
    for (size_t i = 0; i < static_cast<size_t>(toRead); ++i) {
      int32_t left = buffer_[i * 2u];
      int32_t right = buffer_[i * 2u + 1u];
      out[i] = static_cast<float>(left + right) * (0.5f * kInt16ToFloat);
    }
  }

  framePos_ += toRead;
  if (totalFrames_ > 0 && framePos_ >= totalFrames_) {
    atEnd_ = true;
  } else if (gme_track_ended(emu_)) {
    atEnd_ = true;
    if (totalFrames_ == 0) {
      totalFrames_ = framePos_;
    }
  }

  if (framesRead) {
    *framesRead = toRead;
  }
  return true;
}

bool GmeAudioDecoder::seekToFrame(uint64_t frame) {
  if (!emu_) return false;
  int targetMs = framesToMs(frame, sampleRate_);
  gme_err_t seekErr = gme_seek(emu_, targetMs);
  if (seekErr != nullptr) {
    return false;
  }
  framePos_ = frame;
  atEnd_ = false;
  return true;
}

bool GmeAudioDecoder::getTotalFrames(uint64_t* outFrames) const {
  if (!outFrames || !emu_) return false;
  *outFrames = totalFrames_;
  return true;
}
