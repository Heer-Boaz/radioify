#include "vgmaudio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

#if __has_include(<emu/SoundEmu.h>)
#include <emu/SoundEmu.h>
#elif __has_include(<SoundEmu.h>)
#include <SoundEmu.h>
#endif

namespace {
constexpr float kInt16ToFloat = 1.0f / 32768.0f;
constexpr uint32_t kDefaultBufferFrames = 1024;
constexpr uint32_t kSeekChunkFrames = 2048;
constexpr double kSpeedSteps[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
constexpr int kVolumeStepsDb[] = {-12, -6, -3, 0, 3, 6};

void setError(std::string* error, const char* message) {
  if (error && message) {
    *error = message;
  }
}

#ifndef _WIN32
std::string toUtf8String(const std::filesystem::path& p) {
  return p.string();
}
#endif

double speedFromStep(int step) {
  int idx = std::clamp(step, 0, static_cast<int>(std::size(kSpeedSteps)) - 1);
  return kSpeedSteps[idx];
}

int32_t masterVolumeFromStep(int step) {
  int idx = std::clamp(step, 0, static_cast<int>(std::size(kVolumeStepsDb)) - 1);
  double db = static_cast<double>(kVolumeStepsDb[idx]);
  double mult = std::pow(10.0, db / 20.0);
  double fixed = mult * 65536.0;
  if (fixed > static_cast<double>(std::numeric_limits<int32_t>::max())) {
    fixed = static_cast<double>(std::numeric_limits<int32_t>::max());
  }
  return static_cast<int32_t>(std::llround(fixed));
}

uint8_t phaseInvertMask(VgmPhaseInvert mode) {
  switch (mode) {
    case VgmPhaseInvert::Left:
      return 0x01;
    case VgmPhaseInvert::Right:
      return 0x02;
    case VgmPhaseInvert::Both:
      return 0x03;
    case VgmPhaseInvert::Off:
    default:
      return 0x00;
  }
}

uint32_t playbackHzValue(VgmPlaybackHz mode) {
  switch (mode) {
    case VgmPlaybackHz::Hz50:
      return 50;
    case VgmPlaybackHz::Hz60:
      return 60;
    case VgmPlaybackHz::Auto:
    default:
      return 0;
  }
}

int bcdToInt(uint8_t value) {
  return ((value >> 4) & 0x0F) * 10 + (value & 0x0F);
}

std::string formatVgmVersion(uint32_t version) {
  int major = bcdToInt(static_cast<uint8_t>((version >> 8) & 0xFF));
  int minor = bcdToInt(static_cast<uint8_t>(version & 0xFF));
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d.%02d", major, minor);
  return std::string(buf);
}

std::string hex32(uint32_t value) {
  char buf[11];
  std::snprintf(buf, sizeof(buf), "0x%08x", value);
  return std::string(buf);
}

const char* tagLabelForKey(const char* key) {
  if (!key) return nullptr;
  if (std::strcmp(key, "TITLE") == 0) return "Title";
  if (std::strcmp(key, "TITLE-JPN") == 0) return "Title (JPN)";
  if (std::strcmp(key, "GAME") == 0) return "Game";
  if (std::strcmp(key, "GAME-JPN") == 0) return "Game (JPN)";
  if (std::strcmp(key, "SYSTEM") == 0) return "System";
  if (std::strcmp(key, "SYSTEM-JPN") == 0) return "System (JPN)";
  if (std::strcmp(key, "ARTIST") == 0) return "Artist";
  if (std::strcmp(key, "ARTIST-JPN") == 0) return "Artist (JPN)";
  if (std::strcmp(key, "DATE") == 0) return "Date";
  if (std::strcmp(key, "ENCODED_BY") == 0) return "Encoded by";
  if (std::strcmp(key, "COMMENT") == 0) return "Comment";
  return key;
}

std::string deviceDisplayName(const PLR_DEV_INFO& devInfo) {
  const char* name = SndEmu_GetDevName(devInfo.type, 0x00, devInfo.devCfg);
  std::string label = (name && name[0]) ? name : "Unknown";
  if (devInfo.instance > 0) {
    label += " #" + std::to_string(devInfo.instance + 1);
  }
  return label;
}

std::string coreDisplayName(const DEV_DEF* def) {
  if (!def) return "Unknown core";
  std::string name = def->name ? def->name : "Unknown core";
  if (def->coreID != 0) {
    name += " (" + hex32(def->coreID) + ")";
  }
  return name;
}

VGMPlayer* getVgmPlayer(PlayerA* player) {
  if (!player) return nullptr;
  PlayerBase* base = player->GetPlayer();
  if (!base) return nullptr;
  return static_cast<VGMPlayer*>(base);
}
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
  playbackSpeed_ = speedFromStep(options_.speedStep);
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
  player_->SetOutputSettings(sampleRate_, static_cast<uint8_t>(outputChannels_), 16,
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
  playbackSpeed_ = speedFromStep(options_.speedStep);
  double hzSpeed = 1.0;
  VGMPlayer* vgm = getVgmPlayer(player_);
  if (vgm) {
    const VGM_HEADER* header = vgm->GetFileHeader();
    if (header && header->recordHz == 0) {
      if (options_.playbackHz == VgmPlaybackHz::Hz50) {
        hzSpeed = 50.0 / 60.0;
      } else if (options_.playbackHz == VgmPlaybackHz::Hz60) {
        hzSpeed = 1.0;
      }
    }
  }
  playbackSpeed_ *= hzSpeed;
  if (player_) {
    PlayerA::Config config = player_->GetConfiguration();
    config.loopCount = static_cast<uint32_t>(std::max(options_.loopCount, 0));
    config.fadeSmpls =
        static_cast<uint32_t>(std::max(options_.fadeMs, 0) * sampleRate_ / 1000);
    config.endSilenceSmpls =
        static_cast<uint32_t>(std::max(options_.endSilenceMs, 0) * sampleRate_ / 1000);
    config.ignoreVolGain = options_.ignoreVolGain;
    config.chnInvert = phaseInvertMask(options_.phaseInvert);
    config.masterVol = masterVolumeFromStep(options_.masterVolumeStep);
    config.pbSpeed = playbackSpeed_;
    player_->SetConfiguration(config);

    if (vgm) {
      VGM_PLAY_OPTIONS playOpts{};
      vgm->GetPlayerOptions(playOpts);
      playOpts.playbackHz = playbackHzValue(options_.playbackHz);
      playOpts.hardStopOld = options_.hardStopOld ? 1 : 0;
      vgm->SetPlayerOptions(playOpts);
    }
  }
  updateBaseFrames();
  updateTotalFrames();
}

bool VgmAudioDecoder::getMetadata(std::vector<VgmMetadataEntry>* out) const {
  if (!out) return false;
  out->clear();
  if (!player_) return false;

  PlayerBase* base = player_->GetPlayer();
  if (!base) return false;

  const char* const* tags = base->GetTags();
  if (tags) {
    for (const char* const* t = tags; *t; t += 2) {
      if (!t[1] || t[1][0] == '\0') continue;
      const char* label = tagLabelForKey(t[0]);
      out->push_back({label ? label : t[0], t[1]});
    }
  }

  VGMPlayer* vgm = getVgmPlayer(player_);
  if (vgm) {
    const VGM_HEADER* header = vgm->GetFileHeader();
    if (header) {
      out->push_back({"Version", formatVgmVersion(header->fileVer)});
      if (header->recordHz != 0) {
        out->push_back({"Record Hz", std::to_string(header->recordHz)});
      } else {
        out->push_back({"Record Hz", "auto"});
      }
      out->push_back({"Total ticks", std::to_string(header->numTicks)});
      if (header->loopTicks != 0) {
        out->push_back({"Loop ticks", std::to_string(header->loopTicks)});
      }
      if (header->volumeGain != 0) {
        double db = static_cast<double>(header->volumeGain) * 6.0 / 256.0;
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%.1f dB", db);
        out->push_back({"Volume gain", buf});
      }
    }

    PLR_SONG_INFO info{};
    if (vgm->GetSongInfo(info) == 0) {
      out->push_back({"Devices", std::to_string(info.deviceCnt)});
      if (info.tickRateDiv != 0) {
        std::string tickRate = std::to_string(info.tickRateMul) + "/" +
                               std::to_string(info.tickRateDiv);
        out->push_back({"Tick rate", tickRate});
      }
      if (info.songLen != 0) {
        double seconds =
            static_cast<double>(info.songLen) * info.tickRateMul /
            static_cast<double>(info.tickRateDiv);
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.2f s", seconds);
        out->push_back({"Length (1x)", buf});
      }
    }
  }
  return true;
}

bool VgmAudioDecoder::getDevices(std::vector<VgmDeviceInfo>* out) const {
  if (!out) return false;
  out->clear();
  if (!player_) return false;

  PlayerBase* base = player_->GetPlayer();
  if (!base) return false;

  std::vector<PLR_DEV_INFO> devInfos;
  UINT8 ret = base->GetSongDeviceInfo(devInfos);
  if (ret != 0 && ret != 1) return false;

  for (const auto& devInfo : devInfos) {
    if (devInfo.parentIdx != static_cast<uint32_t>(-1)) {
      continue;
    }
    VgmDeviceInfo info;
    info.id = devInfo.id;
    info.instance = devInfo.instance;
    info.name = deviceDisplayName(devInfo);
    if (devInfo.devDecl && devInfo.devDecl->channelCount) {
      info.channelCount =
          devInfo.devDecl->channelCount(devInfo.devCfg);
    }

    const DEV_DECL* decl = devInfo.devDecl;
    if (decl && decl->cores) {
      for (const DEV_DEF* const* def = decl->cores; *def; ++def) {
        info.coreIds.push_back((*def)->coreID);
        info.coreNames.push_back(coreDisplayName(*def));
      }
    } else {
      const DEV_DEF* const* defList =
          SndEmu_GetDevDefList(devInfo.type);
      if (defList) {
        for (const DEV_DEF* const* def = defList; *def; ++def) {
          info.coreIds.push_back((*def)->coreID);
          info.coreNames.push_back(coreDisplayName(*def));
        }
      }
    }
    out->push_back(std::move(info));
  }
  return true;
}

bool VgmAudioDecoder::getDeviceOptions(uint32_t deviceId,
                                       VgmDeviceOptions* out) const {
  if (!out) return false;
  if (!player_) return false;
  PlayerBase* base = player_->GetPlayer();
  if (!base) return false;

  PLR_DEV_OPTS devOpts{};
  if (base->GetDeviceOptions(deviceId, devOpts) != 0) return false;
  out->coreId = devOpts.emuCore[0];
  out->resamplerMode = devOpts.resmplMode;
  out->sampleRateMode = devOpts.srMode;
  out->sampleRate = devOpts.smplRate;
  out->muted = (devOpts.muteOpts.disable & 0x01) != 0;
  return true;
}

bool VgmAudioDecoder::setDeviceOptions(uint32_t deviceId,
                                       const VgmDeviceOptions& options) {
  if (!player_) return false;
  PlayerBase* base = player_->GetPlayer();
  if (!base) return false;

  PLR_DEV_OPTS devOpts{};
  if (base->GetDeviceOptions(deviceId, devOpts) != 0) return false;

  devOpts.emuCore[0] = options.coreId;
  devOpts.resmplMode = options.resamplerMode;
  devOpts.srMode = options.sampleRateMode;
  devOpts.smplRate = options.sampleRate;
  devOpts.muteOpts.disable &= ~0x01;
  if (options.muted) {
    devOpts.muteOpts.disable |= 0x01;
  }

  return base->SetDeviceOptions(deviceId, devOpts) == 0;
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

bool vgmListTracks(const std::filesystem::path& path,
                   std::vector<TrackEntry>* out,
                   std::string* error) {
  if (!out) return false;
  out->clear();

  VgmAudioDecoder decoder;
  if (!decoder.init(path, 2, 44100, error)) {
    return false;
  }
  std::vector<VgmMetadataEntry> metadata;
  decoder.getMetadata(&metadata);

  auto findMeta = [&](const char* key) -> std::string {
    for (const auto& entry : metadata) {
      if (entry.key == key) return entry.value;
    }
    return {};
  };

  std::string title = findMeta("Title");
  if (title.empty()) title = findMeta("Title (JPN)");
  if (title.empty()) title = findMeta("Game");
  if (title.empty()) title = findMeta("Game (JPN)");
  if (title.empty()) title = findMeta("System");
  if (title.empty()) title = findMeta("System (JPN)");

  TrackEntry entry{};
  entry.index = 0;
  entry.lengthMs = -1;
  entry.title = std::move(title);
  out->push_back(std::move(entry));
  return true;
}

bool vgmReadMetadata(const std::filesystem::path& path,
                     std::vector<VgmMetadataEntry>* out,
                     std::string* error) {
  if (!out) return false;
  out->clear();

  VgmAudioDecoder decoder;
  if (!decoder.init(path, 2, 44100, error)) {
    return false;
  }
  if (!decoder.getMetadata(out)) {
    setError(error, "Failed to read VGM metadata.");
    return false;
  }
  return true;
}

bool vgmReadDevices(const std::filesystem::path& path,
                    std::vector<VgmDeviceInfo>* out,
                    std::string* error) {
  if (!out) return false;
  out->clear();

  VgmAudioDecoder decoder;
  if (!decoder.init(path, 2, 44100, error)) {
    return false;
  }
  if (!decoder.getDevices(out)) {
    setError(error, "Failed to read VGM device list.");
    return false;
  }
  return true;
}
