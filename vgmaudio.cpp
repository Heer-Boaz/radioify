#include "vgmaudio.h"

#include <algorithm>
#include <cmath>
#include <limits>

#if __has_include(<player/playera.hpp>)
#include <player/playera.hpp>
#elif __has_include(<player/playera.h>)
#include <player/playera.h>
#endif

#if __has_include(<player/vgmplayer.hpp>)
#include <player/vgmplayer.hpp>
#elif __has_include(<player/vgmplayer.h>)
#include <player/vgmplayer.h>
#endif

#if __has_include(<utils/FileLoader.h>)
#include <utils/FileLoader.h>
#elif __has_include(<utils/fileloader.h>)
#include <utils/fileloader.h>
#endif

namespace {
constexpr float kInt16ToFloat = 1.0f / 32768.0f;
constexpr uint32_t kDefaultBufferFrames = 1024;
constexpr uint32_t kSeekChunkFrames = 2048;

void setError(std::string* error, const char* message) {
  if (error && message) {
    *error = message;
  }
}

double tempoValue(VgmTempoMode mode) {
  return mode == VgmTempoMode::Pal50 ? (50.0 / 60.0) : 1.0;
}

#ifndef _WIN32
std::string toUtf8String(const std::filesystem::path& p) {
  return p.string();
}
#endif
}  // namespace

VgmAudioDecoder::VgmAudioDecoder() = default;
VgmAudioDecoder::~VgmAudioDecoder() { uninit(); }

bool VgmAudioDecoder::init(const std::filesystem::path& path,
                           uint32_t channels, uint32_t sampleRate,
                           std::string* error) {
  uninit();

  if (channels != 1 && channels != 2) {
    setError(error, "Unsupported channel count for VGM.");
    return false;
  }

  file_ = path;
  channels_ = channels;
  sampleRate_ = sampleRate;
  outputChannels_ = 2;
  playbackSpeed_ = tempoValue(options_.tempoMode);
  warning_.clear();

  if (!initPlayer(error)) {
    uninit();
    return false;
  }

  updateBaseFrames();
  updateTotalFrames();
  framePos_ = 0;
  atEnd_ = false;
  active_ = true;
  return true;
}

bool VgmAudioDecoder::initPlayer(std::string* error) {
  destroyPlayer();

  player_ = new PlayerA();
  player_->RegisterPlayerEngine(new VGMPlayer());
  player_->SetOutputSettings(sampleRate_, outputChannels_, 16,
                             kDefaultBufferFrames);
  player_->SetPlaybackSpeed(playbackSpeed_);

  if (!loadFile(file_, error)) {
    return false;
  }
  return true;
}

bool VgmAudioDecoder::loadFile(const std::filesystem::path& path,
                               std::string* error) {
  if (!player_) return false;
  if (loader_) {
    DataLoader_Deinit(loader_);
    loader_ = nullptr;
  }

#ifdef _WIN32
  loader_ = FileLoader_InitW(path.wstring().c_str());
#else
  loader_ = FileLoader_Init(toUtf8String(path).c_str());
#endif
  if (!loader_) {
    setError(error, "Failed to init VGM loader.");
    return false;
  }
  if (DataLoader_Load(loader_) != 0) {
    setError(error, "Failed to open VGM file.");
    return false;
  }

  if (player_->LoadFile(loader_)) {
    setError(error, "Failed to load VGM file.");
    return false;
  }
  if (player_->Start()) {
    setError(error, "Failed to start VGM playback.");
    return false;
  }
  return true;
}

void VgmAudioDecoder::applyOptions(const VgmPlaybackOptions& options) {
  options_ = options;
  playbackSpeed_ = tempoValue(options_.tempoMode);
  if (player_) {
    player_->SetPlaybackSpeed(playbackSpeed_);
  }
  updateBaseFrames();
  updateTotalFrames();
}

void VgmAudioDecoder::updateBaseFrames() {
  baseFrames_ = 0;
  if (!player_) return;
  double totalSec = player_->GetTotalTime(PLAYTIME_LOOP_INCL |
                                          PLAYTIME_TIME_PBK |
                                          PLAYTIME_WITH_FADE |
                                          PLAYTIME_WITH_SLNC);
  if (!std::isfinite(totalSec) || totalSec <= 0.0) return;
  baseFrames_ =
      static_cast<uint64_t>(std::llround(totalSec * sampleRate_));
}

void VgmAudioDecoder::updateTotalFrames() {
  totalFrames_ = baseFrames_;
}

bool VgmAudioDecoder::readFrames(float* out, uint32_t frameCount,
                                 uint64_t* framesRead) {
  if (framesRead) *framesRead = 0;
  if (!active_ || !player_ || !out || frameCount == 0) return false;

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

  uint64_t samplesWanted = toRead * outputChannels_;
  if (samplesWanted > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max() /
                                            sizeof(int16_t))) {
    return false;
  }

  uint32_t bytesWanted =
      static_cast<uint32_t>(samplesWanted * sizeof(int16_t));
  buffer_.resize(static_cast<size_t>(samplesWanted));
  uint32_t bytesRead = player_->Render(bytesWanted, buffer_.data());
  if (bytesRead == 0) {
    atEnd_ = true;
    return true;
  }

  size_t samplesRead = static_cast<size_t>(bytesRead / sizeof(int16_t));
  uint64_t framesDone = samplesRead / outputChannels_;
  if (framesDone == 0) {
    atEnd_ = true;
    return true;
  }

  if (channels_ == 2) {
    size_t samplesOut =
        static_cast<size_t>(framesDone) * outputChannels_;
    for (size_t i = 0; i < samplesOut; ++i) {
      out[i] = static_cast<float>(buffer_[i]) * kInt16ToFloat;
    }
  } else {
    for (size_t i = 0; i < static_cast<size_t>(framesDone); ++i) {
      int32_t left = buffer_[i * 2u];
      int32_t right = buffer_[i * 2u + 1u];
      out[i] = static_cast<float>(left + right) * (0.5f * kInt16ToFloat);
    }
  }

  framePos_ += framesDone;
  if (totalFrames_ > 0 && framePos_ >= totalFrames_) {
    atEnd_ = true;
  } else if (framesDone < toRead) {
    atEnd_ = true;
  }

  if (framesRead) {
    *framesRead = framesDone;
  }
  return true;
}

bool VgmAudioDecoder::resetPlayback(std::string* error) {
  if (file_.empty()) return false;
  if (!initPlayer(error)) {
    return false;
  }
  updateBaseFrames();
  updateTotalFrames();
  framePos_ = 0;
  atEnd_ = false;
  active_ = true;
  return true;
}

bool VgmAudioDecoder::seekToFrame(uint64_t frame) {
  if (!active_ || !player_) return false;
  if (frame == framePos_) return true;

  if (frame < framePos_) {
    std::string error;
    if (!resetPlayback(&error)) {
      return false;
    }
  }

  uint64_t toSkip = frame - framePos_;
  std::vector<float> scratch;
  while (toSkip > 0) {
    uint32_t chunk =
        static_cast<uint32_t>(std::min<uint64_t>(toSkip, kSeekChunkFrames));
    scratch.resize(static_cast<size_t>(chunk) * channels_);
    uint64_t read = 0;
    if (!readFrames(scratch.data(), chunk, &read)) {
      return false;
    }
    if (read == 0) {
      break;
    }
    toSkip -= read;
  }
  return true;
}

bool VgmAudioDecoder::getTotalFrames(uint64_t* outFrames) const {
  if (!outFrames || !player_) return false;
  *outFrames = totalFrames_;
  return true;
}

void VgmAudioDecoder::destroyPlayer() {
  if (player_) {
    delete player_;
    player_ = nullptr;
  }
  if (loader_) {
    DataLoader_Deinit(loader_);
    loader_ = nullptr;
  }
}

void VgmAudioDecoder::uninit() {
  destroyPlayer();
  file_.clear();
  channels_ = 0;
  sampleRate_ = 0;
  baseFrames_ = 0;
  totalFrames_ = 0;
  framePos_ = 0;
  atEnd_ = false;
  active_ = false;
  buffer_.clear();
  warning_.clear();
}

bool vgmListTracks(const std::filesystem::path&,
                   std::vector<TrackEntry>* out,
                   std::string*) {
  if (!out) return false;
  out->clear();
  TrackEntry entry{};
  entry.index = 0;
  entry.lengthMs = -1;
  out->push_back(std::move(entry));
  return true;
}
