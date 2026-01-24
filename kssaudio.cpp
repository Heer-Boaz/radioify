#include "kssaudio.h"

#include <algorithm>
#include <cmath>

namespace {
constexpr float kInt16ToFloat = 1.0f / 32768.0f;
constexpr int kDefaultTrackLengthMs = 150000;
constexpr uint32_t kDefaultFadeMs = 4000;

void setError(std::string* error, const char* message) {
  if (error && message) {
    *error = message;
  }
}

std::string toUtf8String(const std::filesystem::path& path) {
#ifdef _WIN32
  auto u8 = path.u8string();
  return std::string(u8.begin(), u8.end());
#else
  return path.string();
#endif
}

uint64_t msToFrames(int64_t ms, uint32_t sampleRate) {
  if (ms <= 0 || sampleRate == 0) return 0;
  double frames = static_cast<double>(ms) * static_cast<double>(sampleRate) /
                  1000.0;
  return static_cast<uint64_t>(std::llround(frames));
}

const KSSINFO* findTrackInfo(const KSS* kss, int song) {
  if (!kss || !kss->info || kss->info_num <= 0) return nullptr;
  for (uint16_t i = 0; i < kss->info_num; ++i) {
    if (kss->info[i].song == song) {
      return &kss->info[i];
    }
  }
  return nullptr;
}

uint32_t computeFadeMs(const KSSINFO* info) {
  if (info && info->fade_in_ms > 0) {
    return static_cast<uint32_t>(info->fade_in_ms);
  }
  return kDefaultFadeMs;
}
}  // namespace

KssAudioDecoder::KssAudioDecoder() = default;
KssAudioDecoder::~KssAudioDecoder() { uninit(); }

bool KssAudioDecoder::init(const std::filesystem::path& path,
                           uint32_t channels, uint32_t sampleRate,
                           std::string* error, int trackIndex,
                           bool force50Hz) {
  uninit();

  if (channels != 1 && channels != 2) {
    setError(error, "Unsupported channel count for KSS.");
    return false;
  }

  std::string pathUtf8 = toUtf8String(path);
  std::vector<char> pathBuf(pathUtf8.begin(), pathUtf8.end());
  pathBuf.push_back('\0');

  KSS* kss = KSS_load_file(pathBuf.data());
  if (!kss) {
    setError(error, "Failed to load KSS file.");
    return false;
  }

  int trackBase = kss->trk_min;
  int trackCount = (kss->trk_max >= kss->trk_min)
                       ? (kss->trk_max - kss->trk_min + 1)
                       : 0;
  if (trackCount <= 0) {
    KSS_delete(kss);
    setError(error, "No tracks found in KSS file.");
    return false;
  }

  if (trackIndex < 0) trackIndex = 0;
  if (trackIndex >= trackCount) trackIndex = trackCount - 1;
  int song = trackBase + trackIndex;

  KSSPLAY* kssplay = KSSPLAY_new(sampleRate, channels, 16);
  if (!kssplay) {
    KSS_delete(kss);
    setError(error, "Failed to initialize KSS player.");
    return false;
  }

  if (KSSPLAY_set_data(kssplay, kss) != 0) {
    KSSPLAY_delete(kssplay);
    KSS_delete(kss);
    setError(error, "Failed to load KSS data.");
    return false;
  }

  force50Hz_ = force50Hz;
  kssplay->vsync_freq = force50Hz ? 50.0 : 0.0;
  KSSPLAY_reset(kssplay, static_cast<uint32_t>(song), 0);

  const KSSINFO* info = findTrackInfo(kss, song);
  int lengthMs = (info && info->time_in_ms > 0) ? info->time_in_ms
                                                : kDefaultTrackLengthMs;
  fadeDurationMs_ = computeFadeMs(info);
  totalFrames_ = msToFrames(lengthMs, sampleRate);
  if (totalFrames_ > 0) {
    uint64_t fadeFrames = msToFrames(fadeDurationMs_, sampleRate);
    if (fadeFrames >= totalFrames_) {
      fadeStartFrame_ = totalFrames_;
    } else {
      fadeStartFrame_ = totalFrames_ - fadeFrames;
    }
  } else {
    fadeStartFrame_ = 0;
  }

  kss_ = kss;
  kssplay_ = kssplay;
  channels_ = channels;
  sampleRate_ = sampleRate;
  trackBase_ = trackBase;
  trackCount_ = trackCount;
  trackIndex_ = trackIndex;
  framePos_ = 0;
  atEnd_ = false;
  fadeArmed_ = false;
  buffer_.clear();
  return true;
}

void KssAudioDecoder::uninit() {
  if (kssplay_) {
    KSSPLAY_delete(kssplay_);
    kssplay_ = nullptr;
  }
  if (kss_) {
    KSS_delete(kss_);
    kss_ = nullptr;
  }
  channels_ = 0;
  sampleRate_ = 0;
  totalFrames_ = 0;
  framePos_ = 0;
  atEnd_ = false;
  fadeArmed_ = false;
  fadeStartFrame_ = 0;
  fadeDurationMs_ = 0;
  buffer_.clear();
  trackBase_ = 0;
  trackCount_ = 0;
  trackIndex_ = 0;
  force50Hz_ = false;
}

bool KssAudioDecoder::readFrames(float* out, uint32_t frameCount,
                                 uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!kssplay_ || !out || frameCount == 0) return false;
  if (atEnd_) return true;

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

  buffer_.resize(static_cast<size_t>(toRead) * channels_);
  if (!fadeArmed_ && totalFrames_ > 0 &&
      framePos_ < fadeStartFrame_ && framePos_ + toRead > fadeStartFrame_) {
    uint64_t preFade = fadeStartFrame_ - framePos_;
    KSSPLAY_calc(kssplay_, buffer_.data(), static_cast<uint32_t>(preFade));
    KSSPLAY_fade_start(kssplay_, fadeDurationMs_);
    fadeArmed_ = true;
    KSSPLAY_calc(kssplay_,
                 buffer_.data() + static_cast<size_t>(preFade) * channels_,
                 static_cast<uint32_t>(toRead - preFade));
  } else {
    if (!fadeArmed_ && totalFrames_ > 0 && framePos_ >= fadeStartFrame_) {
      KSSPLAY_fade_start(kssplay_, fadeDurationMs_);
      fadeArmed_ = true;
    }
    KSSPLAY_calc(kssplay_, buffer_.data(), static_cast<uint32_t>(toRead));
  }

  if (channels_ == 2) {
    size_t samples = static_cast<size_t>(toRead) * 2u;
    for (size_t i = 0; i < samples; ++i) {
      out[i] = static_cast<float>(buffer_[i]) * kInt16ToFloat;
    }
  } else {
    for (size_t i = 0; i < static_cast<size_t>(toRead); ++i) {
      out[i] = static_cast<float>(buffer_[i]) * kInt16ToFloat;
    }
  }

  framePos_ += toRead;
  if (totalFrames_ > 0 && framePos_ >= totalFrames_) {
    atEnd_ = true;
  } else if (KSSPLAY_get_stop_flag(kssplay_)) {
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

bool KssAudioDecoder::seekToFrame(uint64_t frame) {
  if (!kssplay_ || !kss_) return false;
  if (totalFrames_ > 0 && frame > totalFrames_) {
    frame = totalFrames_;
  }

  int song = trackBase_ + trackIndex_;
  kssplay_->vsync_freq = force50Hz_ ? 50.0 : 0.0;
  KSSPLAY_reset(kssplay_, static_cast<uint32_t>(song), 0);
  framePos_ = 0;
  atEnd_ = false;
  fadeArmed_ = false;
  if (frame == 0) return true;

  uint64_t remaining = frame;
  constexpr uint32_t kChunk = 4096;
  while (remaining > 0) {
    uint32_t chunk =
        remaining > kChunk ? kChunk : static_cast<uint32_t>(remaining);
    KSSPLAY_calc_silent(kssplay_, chunk);
    remaining -= chunk;
  }
  framePos_ = frame;
  if (totalFrames_ > 0 && framePos_ >= totalFrames_) {
    atEnd_ = true;
  }
  return true;
}

bool KssAudioDecoder::getTotalFrames(uint64_t* outFrames) const {
  if (!outFrames || !kssplay_) return false;
  *outFrames = totalFrames_;
  return true;
}

bool kssListTracks(const std::filesystem::path& path,
                   std::vector<TrackEntry>* out,
                   std::string* error) {
  if (!out) return false;
  out->clear();

  std::string pathUtf8 = toUtf8String(path);
  std::vector<char> pathBuf(pathUtf8.begin(), pathUtf8.end());
  pathBuf.push_back('\0');

  KSS* kss = KSS_load_file(pathBuf.data());
  if (!kss) {
    setError(error, "Failed to load KSS file.");
    return false;
  }

  int trackBase = kss->trk_min;
  int trackCount = (kss->trk_max >= kss->trk_min)
                       ? (kss->trk_max - kss->trk_min + 1)
                       : 0;
  if (trackCount <= 0) {
    KSS_delete(kss);
    setError(error, "No tracks found in KSS file.");
    return false;
  }

  out->reserve(static_cast<size_t>(trackCount));
  for (int i = 0; i < trackCount; ++i) {
    TrackEntry entry{};
    entry.index = i;
    entry.lengthMs = -1;

    int song = trackBase + i;
    const KSSINFO* info = findTrackInfo(kss, song);
    if (info) {
      if (info->title[0] != '\0') {
        entry.title = info->title;
      }
      if (info->time_in_ms > 0) {
        entry.lengthMs = info->time_in_ms;
      }
    }

    out->push_back(std::move(entry));
  }

  KSS_delete(kss);
  return true;
}
