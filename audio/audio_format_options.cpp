#include "audio_format_options.h"

#include "audioplayback_internal.h"
#include "audio_stream.h"

#include "media_formats.h"
#include "playback_backend.h"

#include <cmath>

constexpr int kVgmSpeedStepCount = 6;
constexpr int kVgmVolumeStepCount = 6;
constexpr int kVgmLoopSteps[] = {0, 1, 2, 3, 4, 6, 8, 12, 16};
constexpr int kVgmFadeStepsMs[] = {0, 1000, 2000, 3000, 5000, 8000, 10000,
                                   15000};
constexpr int kVgmEndSilenceStepsMs[] = {0, 250, 500, 1000, 2000, 3000, 5000};
constexpr uint32_t kVgmSampleRateSteps[] = {
    0, 11025, 22050, 32000, 44100, 48000, 88200, 96000};

template <typename T>
int findIndex(const T* values, size_t count, T value) {
  for (size_t i = 0; i < count; ++i) {
    if (values[i] == value) return static_cast<int>(i);
  }
  return 0;
}

int advanceIndex(int idx, int count, int direction) {
  if (count <= 0) return 0;
  if (direction > 0) {
    ++idx;
    if (idx >= count) idx = 0;
  } else {
    --idx;
    if (idx < 0) idx = count - 1;
  }
  return idx;
}

float clampHz(float hz, uint32_t sampleRate) {
  float maxHz = static_cast<float>(sampleRate) * 0.45f;
  if (!std::isfinite(hz) || hz <= 0.0f) return 440.0f;
  if (hz < 20.0f) return 20.0f;
  if (hz > maxHz) return maxHz;
  return hz;
}

uint32_t fnv1a32(const uint8_t* data, size_t size) {
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < size; ++i) {
    hash ^= data[i];
    hash *= 16777619u;
  }
  return hash;
}

bool buildPsgAuditionTone(const KssInstrumentProfile& profile,
                          uint32_t sampleRate, KssPsgType psgType,
                          AuditionTone* out) {
  if (!out || profile.device != KssInstrumentDevice::Psg) return false;
  if (profile.data.size() < 5) return false;

  const uint8_t mix = profile.data[0];
  uint8_t volReg = profile.data[1];
  const uint8_t envLo = profile.data[2];
  const uint8_t envHi = profile.data[3];
  const uint8_t envShape = profile.data[4];
  uint8_t noisePeriod = 16;
  if (profile.data.size() >= 6) {
    noisePeriod = static_cast<uint8_t>(profile.data[5] & 0x1f);
    if (noisePeriod == 0) noisePeriod = 1;
  }
  bool useEnv = (volReg & 0x10) != 0;
  uint8_t volume = volReg & 0x0f;
  if (useEnv && envLo == 0 && envHi == 0) {
    useEnv = false;
  }
  if (!useEnv) {
    if (volume == 0) volume = 0x0f;
    volReg = volume;
  }

  PSG* psg = PSG_new(kMsxClockHz, sampleRate);
  if (!psg) return false;
  PSG_reset(psg);
  PSG_setClockDivider(psg, 1);
  switch (psgType) {
    case KssPsgType::Ay:
      PSG_setVolumeMode(psg, 2);
      break;
    case KssPsgType::Ym:
      PSG_setVolumeMode(psg, 1);
      break;
    case KssPsgType::Auto:
    default:
      PSG_setVolumeMode(psg, 0);
      break;
  }

  float freqHz = clampHz(440.0f, sampleRate);
  uint16_t period =
      static_cast<uint16_t>(std::llround(static_cast<double>(kMsxClockHz) /
                                         (16.0 * freqHz)));
  if (period == 0) period = 1;

  const int channel = 0;
  PSG_writeReg(psg, 0, static_cast<uint8_t>(period & 0xff));
  PSG_writeReg(psg, 1, static_cast<uint8_t>((period >> 8) & 0x0f));
  PSG_writeReg(psg, 6, noisePeriod);

  uint8_t mixer = 0x3f;
  if ((mix & 0x01) == 0) mixer &= ~(1 << channel);
  if ((mix & 0x02) == 0) mixer &= ~(1 << (channel + 3));
  PSG_writeReg(psg, 7, mixer);

  PSG_writeReg(psg, 8, useEnv ? static_cast<uint8_t>(0x10 | volume) : volReg);
  PSG_writeReg(psg, 9, 0);
  PSG_writeReg(psg, 10, 0);
  if (useEnv) {
    PSG_writeReg(psg, 11, envLo);
    PSG_writeReg(psg, 12, envHi);
    PSG_writeReg(psg, 13, envShape);
  } else {
    PSG_writeReg(psg, 11, 0);
    PSG_writeReg(psg, 12, 0);
    PSG_writeReg(psg, 13, 0);
  }

  out->kind = AuditionKind::Psg;
  out->psg = psg;
  out->scc = nullptr;
  out->gain = kAuditionGain;
  return true;
}

bool buildSccAuditionTone(const KssInstrumentProfile& profile,
                          uint32_t sampleRate, KssSccType sccType,
                          KssQuality sccQuality, AuditionTone* out) {
  if (!out || profile.device != KssInstrumentDevice::Scc) return false;
  if (profile.data.size() < 32) return false;

  SCC* scc = SCC_new(kMsxClockHz, sampleRate);
  if (!scc) return false;
  SCC_reset(scc);
  SCC_set_rate(scc, sampleRate);
  SCC_set_quality(scc, sccQuality == KssQuality::High ? 1u : 0u);
  switch (sccType) {
    case KssSccType::Standard:
      SCC_set_type(scc, SCC_STANDARD);
      break;
    case KssSccType::Enhanced:
      SCC_set_type(scc, SCC_ENHANCED);
      break;
    case KssSccType::Auto:
    default:
      SCC_set_type(scc, SCC_ENHANCED);
      break;
  }

  for (size_t i = 0; i < 32; ++i) {
    SCC_writeReg(scc, static_cast<uint32_t>(i), profile.data[i]);
  }

  float freqHz = clampHz(440.0f, sampleRate);
  double raw = static_cast<double>(kMsxClockHz) / (32.0 * freqHz) - 1.0;
  uint32_t freqReg = raw <= 0.0 ? 0 : static_cast<uint32_t>(std::llround(raw));
  freqReg = std::min<uint32_t>(freqReg, 0xFFFu);
  if (freqReg < 9) freqReg = 9;
  SCC_writeReg(scc, 0xC0, freqReg & 0xFF);
  SCC_writeReg(scc, 0xC1, (freqReg >> 8) & 0x0F);

  uint8_t volume = profile.volume & 0x0F;
  if (volume == 0) volume = 0x0F;
  SCC_writeReg(scc, 0xD0, volume);
  SCC_writeReg(scc, 0xE1, 0x01);

  out->kind = AuditionKind::SccWave;
  out->psg = nullptr;
  out->scc = scc;
  out->gain = kAuditionGain;
  return true;
}

static void reloadKssWithOptions(AudioPlaybackState& audio) {
  if (!audio.decoderReady || !isAudioMode(audio, AudioMode::Kss)) return;
  uint64_t resumeFrame = audio.state.framesPlayed.load();
  bool wasPaused = audio.state.paused.load();
  if (audio_playback::fileStartSucceeded(
          loadFileAt(audio, audio.nowPlaying, resumeFrame,
                     audio.trackIndex))) {
    if (wasPaused) {
      audio.state.paused.store(true);
    }
  }
}

static void reloadNsfWithOptions(AudioPlaybackState& audio);

void audioToggle50Hz(AudioPlaybackState& audio) {
  if (!audioSupports50HzToggle(audio)) {
    return;
  }
  switch (currentAudioMode(audio)) {
    case AudioMode::Vgm: {
      audio.vgmOptions.playbackHz =
          (audio.vgmOptions.playbackHz == VgmPlaybackHz::Hz50)
              ? VgmPlaybackHz::Hz60
              : VgmPlaybackHz::Hz50;
      audio.state.vgm.applyOptions(audio.vgmOptions);
      uint64_t totalFrames = 0;
      if (audio.state.vgm.getTotalFrames(&totalFrames)) {
        audio.state.totalFrames.store(totalFrames);
      } else {
        audio.state.totalFrames.store(0);
      }
      return;
    }
    case AudioMode::Gme:
      audio.nsfOptions.tempoMode =
          (audio.nsfOptions.tempoMode == NsfTempoMode::Pal50)
              ? NsfTempoMode::Normal
              : NsfTempoMode::Pal50;
      reloadNsfWithOptions(audio);
      return;
    case AudioMode::Kss:
      audio.kssOptions.force50Hz = !audio.kssOptions.force50Hz;
      reloadKssWithOptions(audio);
      return;
    default:
      return;
  }
}

bool audioIs50HzEnabled(const AudioPlaybackState& audio) {
  switch (currentAudioMode(audio)) {
    case AudioMode::Vgm:
      return audio.vgmOptions.playbackHz == VgmPlaybackHz::Hz50;
    case AudioMode::Gme:
      return audio.nsfOptions.tempoMode == NsfTempoMode::Pal50;
    case AudioMode::Kss:
      return audio.kssOptions.force50Hz;
    default:
      return false;
  }
}

bool audioSupports50HzToggle(const AudioPlaybackState& audio) {
  const AudioMode mode = currentAudioMode(audio);
  return mode == AudioMode::Kss || mode == AudioMode::Gme ||
         mode == AudioMode::Vgm;
}

KssPlaybackOptions audioGetKssOptionState(const AudioPlaybackState& audio) {
  return audio.kssOptions;
}

static bool toKssDevice(KssInstrumentDevice device, KSS_DEVICE* out) {
  if (!out) return false;
  switch (device) {
    case KssInstrumentDevice::Psg:
      *out = KSS_DEVICE_PSG;
      return true;
    case KssInstrumentDevice::Scc:
      *out = KSS_DEVICE_SCC;
      return true;
    case KssInstrumentDevice::Opll:
      *out = KSS_DEVICE_OPLL;
      return true;
    case KssInstrumentDevice::None:
    default:
      break;
  }
  return false;
}

bool audioGetKssInstrumentRegs(AudioPlaybackState& audio,
                               KssInstrumentDevice device,
                               std::vector<uint8_t>* out) {
  if (!out) return false;
  if (!isAudioMode(audio, AudioMode::Kss)) return false;
  if (!audio.state.kss.active()) return false;
  KSS_DEVICE kssDevice{};
  if (!toKssDevice(device, &kssDevice)) return false;
  return audio.state.kss.readDeviceRegs(kssDevice, out);
}

bool audioSetKssInstrumentPreview(AudioPlaybackState& audio,
                                  KssInstrumentDevice device, int channel) {
  bool changed = false;
  int maxChannels = 0;
  switch (device) {
    case KssInstrumentDevice::Psg:
      maxChannels = 3;
      break;
    case KssInstrumentDevice::Scc:
      maxChannels = 5;
      break;
    case KssInstrumentDevice::Opll:
      maxChannels = 9;
      break;
    case KssInstrumentDevice::None:
    default:
      break;
  }

  if (device == KssInstrumentDevice::None || channel < 0 ||
      (maxChannels > 0 && channel >= maxChannels)) {
    if (audio.kssOptions.instrumentDevice != KssInstrumentDevice::None ||
        audio.kssOptions.instrumentChannel != -1) {
      audio.kssOptions.instrumentDevice = KssInstrumentDevice::None;
      audio.kssOptions.instrumentChannel = -1;
      changed = true;
    }
  } else if (audio.kssOptions.instrumentDevice != device ||
             audio.kssOptions.instrumentChannel != channel) {
    audio.kssOptions.instrumentDevice = device;
    audio.kssOptions.instrumentChannel = channel;
    changed = true;
  }

  if (changed) {
    reloadKssWithOptions(audio);
  }
  return changed;
}

bool audioGetKssInstrumentAuditionState(const AudioPlaybackState& audio,
                                        KssInstrumentDevice* device,
                                        uint32_t* hash) {
  if (device) *device = audio.audition.device;
  if (hash) *hash = audio.audition.hash;
  return audio.audition.active.load();
}

bool audioStartKssInstrumentAudition(
    AudioPlaybackState& audio, const KssInstrumentProfile& profile) {
  if (profile.device != KssInstrumentDevice::Psg &&
      profile.device != KssInstrumentDevice::Scc) {
    return false;
  }

  AuditionTone tone;
  bool ok = false;
  if (profile.device == KssInstrumentDevice::Psg) {
    ok = buildPsgAuditionTone(profile, audio.sampleRate,
                              audio.kssOptions.psgType, &tone);
  } else if (profile.device == KssInstrumentDevice::Scc) {
    ok = buildSccAuditionTone(profile, audio.sampleRate,
                              audio.kssOptions.sccType,
                              audio.kssOptions.sccQuality, &tone);
  }
  if (!ok) return false;

  if (audio.audition.active.load()) {
    stopAuditionWorker(audio);
    if (audio.state.externalStream.load()) {
      audioStreamReset(audio, 0);
    }
  } else {
    audio.audition.resumeValid =
        audio.decoderReady && !audio.nowPlaying.empty() &&
        !audio.state.externalStream.load();
    if (audio.audition.resumeValid) {
      audio.audition.resumeFile = audio.nowPlaying;
      audio.audition.resumeFrame = audio.state.framesPlayed.load();
      audio.audition.resumePaused = audio.state.paused.load();
      audio.audition.resumeTrackIndex = audio.trackIndex;
    }
    if (!audioStartStream(audio, 0)) {
      if (audio.audition.resumeValid) {
        loadFileAt(audio, audio.audition.resumeFile,
                   audio.audition.resumeFrame,
                   audio.audition.resumeTrackIndex);
        if (audio.audition.resumePaused) {
          audio.state.paused.store(true);
        }
      }
      audio.audition.resumeValid = false;
      return false;
    }
  }

  audio.audition.device = profile.device;
  audio.audition.hash = profile.hash;
  startAuditionWorker(audio, std::move(tone));
  return true;
}

bool audioStopKssInstrumentAudition(AudioPlaybackState& audio) {
  if (!audio.audition.active.load()) return false;
  stopAuditionWorker(audio);
  audio.audition.device = KssInstrumentDevice::None;
  audio.audition.hash = 0;

  if (audio.audition.resumeValid) {
    const bool resumed = audio_playback::fileStartSucceeded(loadFileAt(
        audio, audio.audition.resumeFile, audio.audition.resumeFrame,
        audio.audition.resumeTrackIndex));
    if (resumed && audio.audition.resumePaused) {
      audio.state.paused.store(true);
    }
    audio.audition.resumeValid = false;
    return resumed;
  }
  audioStopStream(audio);
  audio.audition.resumeValid = false;
  return true;
}

bool audioScanKssInstruments(const std::filesystem::path& file, int trackIndex,
                             uint32_t sampleRate,
                             KssPlaybackOptions options,
                             const std::function<bool()>& cancellationRequested,
                             std::vector<KssInstrumentProfile>* out,
                             std::string* error) {
  if (!out) return false;
  out->clear();
  if (!isKssExt(file)) return false;

  options.instrumentDevice = KssInstrumentDevice::None;
  options.instrumentChannel = -1;

  KssAudioDecoder decoder;
  if (!decoder.init(file, 1, sampleRate, error, trackIndex, options)) {
    return false;
  }

  uint64_t totalFrames = 0;
  decoder.getTotalFrames(&totalFrames);
  if (totalFrames == 0) {
    totalFrames = static_cast<uint64_t>(sampleRate) * 150;
  }

  std::unordered_map<std::string, size_t> seen;

  auto addPsgInstrument = [&](const uint8_t* key, size_t keySize,
                              const uint8_t* data, size_t dataSize,
                              uint8_t volume, bool envUsed) {
    std::string mapKey;
    mapKey.reserve(keySize + 1);
    mapKey.push_back('P');
    mapKey.append(reinterpret_cast<const char*>(key), keySize);
    auto it = seen.find(mapKey);
    if (it != seen.end()) {
      KssInstrumentProfile& existing = (*out)[it->second];
      if (dataSize >= 6 && existing.data.size() >= 6) {
        bool existingEnv = (existing.data[1] & 0x10) != 0;
        if (envUsed && existingEnv) {
          uint16_t envPeriod =
              static_cast<uint16_t>(data[2]) |
              (static_cast<uint16_t>(data[3]) << 8);
          uint16_t existingPeriod =
              static_cast<uint16_t>(existing.data[2]) |
              (static_cast<uint16_t>(existing.data[3]) << 8);
          if (envPeriod != 0 && existingPeriod == 0) {
            existing.data[2] = data[2];
            existing.data[3] = data[3];
          }
        }
        if (!envUsed && !existingEnv) {
          uint8_t existingVolume = existing.data[1] & 0x0f;
          if ((volume & 0x0f) > existingVolume) {
            existing.data[1] = static_cast<uint8_t>(volume & 0x0f);
          }
        }
        bool noiseEnabled = (data[0] & 0x2) == 0;
        if (noiseEnabled && existing.data[5] == 0 && data[5] != 0) {
          existing.data[5] = data[5];
        }
      }
      if (volume > existing.volume) existing.volume = volume;
      return;
    }

    KssInstrumentProfile profile;
    profile.device = KssInstrumentDevice::Psg;
    profile.data.assign(data, data + dataSize);
    profile.hash = fnv1a32(
        reinterpret_cast<const uint8_t*>(mapKey.data()), mapKey.size());
    profile.volume = volume;
    seen.emplace(std::move(mapKey), out->size());
    out->push_back(std::move(profile));
  };

  auto addSccInstrument = [&](const uint8_t* wave, size_t waveSize,
                              uint8_t volume) {
    std::string mapKey;
    mapKey.reserve(waveSize + 1);
    mapKey.push_back('S');
    mapKey.append(reinterpret_cast<const char*>(wave), waveSize);
    auto it = seen.find(mapKey);
    if (it != seen.end()) {
      KssInstrumentProfile& existing = (*out)[it->second];
      if (volume > existing.volume) existing.volume = volume;
      return;
    }

    KssInstrumentProfile profile;
    profile.device = KssInstrumentDevice::Scc;
    profile.data.assign(wave, wave + waveSize);
    profile.hash = fnv1a32(profile.data.data(), profile.data.size());
    profile.volume = volume;
    seen.emplace(std::move(mapKey), out->size());
    out->push_back(std::move(profile));
  };

  auto scanRegs = [&]() {
    std::vector<uint8_t> psgRegs;
    if (decoder.readDeviceRegs(KSS_DEVICE_PSG, &psgRegs) &&
        psgRegs.size() >= 14) {
      uint8_t mixer = psgRegs[7];
      for (int ch = 0; ch < 3; ++ch) {
        uint8_t toneDisable = (mixer >> ch) & 0x1;
        uint8_t noiseDisable = (mixer >> (ch + 3)) & 0x1;
        bool active = (toneDisable == 0) || (noiseDisable == 0);
        uint8_t volReg = psgRegs[static_cast<size_t>(8 + ch)];
        bool env = (volReg & 0x10) != 0;
        if (env && psgRegs[11] == 0 && psgRegs[12] == 0) {
          env = false;
        }
        if (!active) continue;
        if ((volReg & 0x0f) == 0 && !env) continue;

        uint8_t mixKey = static_cast<uint8_t>(
            (toneDisable ? 1 : 0) | (noiseDisable ? 2 : 0));
        uint8_t envShape = static_cast<uint8_t>(psgRegs[13] & 0x0f);
        uint8_t envKey =
            static_cast<uint8_t>((env ? 0x10 : 0x00) | (env ? envShape : 0));
        uint8_t noiseBucket = 0xFF;
        if (noiseDisable == 0) {
          noiseBucket = static_cast<uint8_t>(
              (psgRegs[6] & 0x1f) / 4);
        }
        uint8_t key[3] = {mixKey, envKey, noiseBucket};
        uint8_t data[6] = {
            mixKey,
            static_cast<uint8_t>((env ? 0x10 : 0x00) | (volReg & 0x0f)),
            psgRegs[11],
            psgRegs[12],
            envShape,
            static_cast<uint8_t>(psgRegs[6] & 0x1f),
        };
        addPsgInstrument(key, sizeof(key), data, sizeof(data),
                         static_cast<uint8_t>(volReg & 0x0f), env);
      }
    }

    std::vector<uint8_t> sccRegs;
    if (decoder.readDeviceRegs(KSS_DEVICE_SCC, &sccRegs) &&
        sccRegs.size() >= 0xE0) {
      for (int ch = 0; ch < 5; ++ch) {
        const uint8_t* wave = sccRegs.data() + ch * 32;
        bool allZero = true;
        for (int i = 0; i < 32; ++i) {
          if (wave[i] != 0) {
            allZero = false;
            break;
          }
        }
        if (allZero) continue;
        uint8_t volume =
            sccRegs[0xD0 + static_cast<size_t>(ch)] & 0x0f;
        addSccInstrument(wave, 32, volume);
      }
    }
  };

  scanRegs();
  const uint32_t chunkFrames = 2048;
  std::vector<float> buffer(chunkFrames);
  uint64_t processed = 0;
  while (processed < totalFrames) {
    if (cancellationRequested && cancellationRequested()) {
      return false;
    }
    uint32_t toRead = static_cast<uint32_t>(
        std::min<uint64_t>(chunkFrames, totalFrames - processed));
    uint64_t read = 0;
    if (!decoder.readFrames(buffer.data(), toRead, &read) || read == 0) {
      break;
    }
    processed += read;
    scanRegs();
  }
  return true;
}

bool audioScanVgmMetadata(const std::filesystem::path& file,
                          std::vector<VgmMetadataEntry>* out,
                          std::string* error) {
  if (!out) return false;
  out->clear();
  if (!isVgmExt(file)) return false;
  return vgmReadMetadata(file, out, error);
}

bool audioScanVgmDevices(const std::filesystem::path& file, uint32_t channels,
                         uint32_t sampleRate, VgmDeviceCatalog* out,
                         std::string* error) {
  if (!out) return false;
  out->devices.clear();
  out->defaults.clear();
  if (!isVgmExt(file)) return false;

  VgmAudioDecoder decoder;
  if (channels == 0) {
    channels = 2;
  }
  if (sampleRate == 0) {
    sampleRate = 48000;
  }
  if (!decoder.init(file, channels, sampleRate, error)) {
    return false;
  }
  if (!decoder.getDevices(&out->devices)) {
    if (error) *error = "Failed to scan VGM devices.";
    return false;
  }

  for (const auto& device : out->devices) {
    VgmDeviceOptions options{};
    if (decoder.getDeviceOptions(device.id, &options)) {
      out->defaults[device.id] = options;
    }
  }
  return true;
}

bool audioAdjustKssOption(AudioPlaybackState& audio, KssOptionId id,
                          int direction) {
  if (direction == 0) return false;
  bool changed = false;
  switch (id) {
    case KssOptionId::Force50Hz:
      audio.kssOptions.force50Hz = !audio.kssOptions.force50Hz;
      changed = true;
      break;
    case KssOptionId::SccType: {
      int next = static_cast<int>(audio.kssOptions.sccType) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      audio.kssOptions.sccType = static_cast<KssSccType>(next);
      changed = true;
      break;
    }
    case KssOptionId::PsgType: {
      int next = static_cast<int>(audio.kssOptions.psgType) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      audio.kssOptions.psgType = static_cast<KssPsgType>(next);
      changed = true;
      break;
    }
    case KssOptionId::OpllType: {
      int next = static_cast<int>(audio.kssOptions.opllType) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      audio.kssOptions.opllType = static_cast<KssOpllType>(next);
      changed = true;
      break;
    }
    case KssOptionId::PsgQuality: {
      int next = static_cast<int>(audio.kssOptions.psgQuality) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      audio.kssOptions.psgQuality = static_cast<KssQuality>(next);
      changed = true;
      break;
    }
    case KssOptionId::SccQuality: {
      int next = static_cast<int>(audio.kssOptions.sccQuality) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      audio.kssOptions.sccQuality = static_cast<KssQuality>(next);
      changed = true;
      break;
    }
    case KssOptionId::OpllStereo:
      audio.kssOptions.opllStereo = !audio.kssOptions.opllStereo;
      changed = true;
      break;
    case KssOptionId::MutePsg:
      audio.kssOptions.mutePsg = !audio.kssOptions.mutePsg;
      changed = true;
      break;
    case KssOptionId::MuteScc:
      audio.kssOptions.muteScc = !audio.kssOptions.muteScc;
      changed = true;
      break;
    case KssOptionId::MuteOpll:
      audio.kssOptions.muteOpll = !audio.kssOptions.muteOpll;
      changed = true;
      break;
    default:
      break;
  }
  if (changed) {
    reloadKssWithOptions(audio);
  }
  return changed;
}

NsfPlaybackOptions audioGetNsfOptionState(const AudioPlaybackState& audio) {
  return audio.nsfOptions;
}

static void reloadNsfWithOptions(AudioPlaybackState& audio) {
  if (!audio.decoderReady || !isAudioMode(audio, AudioMode::Gme)) return;
  if (!isGmeExt(audio.nowPlaying)) return;
  uint64_t resumeFrame = audio.state.framesPlayed.load();
  bool wasPaused = audio.state.paused.load();
  if (audio_playback::fileStartSucceeded(
          loadFileAt(audio, audio.nowPlaying, resumeFrame,
                     audio.trackIndex))) {
    if (wasPaused) {
      audio.state.paused.store(true);
    }
  }
}

bool audioAdjustNsfOption(AudioPlaybackState& audio, NsfOptionId id,
                          int direction) {
  if (direction == 0) return false;
  bool changed = false;
  switch (id) {
    case NsfOptionId::EqPreset: {
      int next = static_cast<int>(audio.nsfOptions.eqPreset) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 1;
      if (next > 1) next = 0;
      audio.nsfOptions.eqPreset = static_cast<NsfEqPreset>(next);
      changed = true;
      break;
    }
    case NsfOptionId::StereoDepth: {
      int next = static_cast<int>(audio.nsfOptions.stereoDepth) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      audio.nsfOptions.stereoDepth = static_cast<NsfStereoDepth>(next);
      changed = true;
      break;
    }
    case NsfOptionId::IgnoreSilence:
      audio.nsfOptions.ignoreSilence = !audio.nsfOptions.ignoreSilence;
      changed = true;
      break;
    case NsfOptionId::TempoMode:
      audio.nsfOptions.tempoMode =
          (audio.nsfOptions.tempoMode == NsfTempoMode::Pal50)
              ? NsfTempoMode::Normal
              : NsfTempoMode::Pal50;
      changed = true;
      break;
    default:
      break;
  }
  if (changed) {
    reloadNsfWithOptions(audio);
  }
  return changed;
}

VgmPlaybackOptions audioGetVgmOptionState(const AudioPlaybackState& audio) {
  return audio.vgmOptions;
}

bool audioGetVgmDeviceOptions(const AudioPlaybackState& audio,
                              uint32_t deviceId, VgmDeviceOptions* out) {
  if (!out) return false;
  if (isAudioMode(audio, AudioMode::Vgm)) {
    if (audio.state.vgm.getDeviceOptions(deviceId, out)) {
      return true;
    }
  }
  auto overrideIt = audio.vgmDeviceOverrides.find(deviceId);
  if (overrideIt == audio.vgmDeviceOverrides.end()) {
    return false;
  }
  *out = overrideIt->second;
  return true;
}

static void reloadVgmWithOptions(AudioPlaybackState& audio) {
  if (!audio.decoderReady || !isAudioMode(audio, AudioMode::Vgm)) return;
  if (!isVgmExt(audio.nowPlaying)) return;
  uint64_t resumeFrame = audio.state.framesPlayed.load();
  bool wasPaused = audio.state.paused.load();
  if (audio_playback::fileStartSucceeded(
          loadFileAt(audio, audio.nowPlaying, resumeFrame,
                     audio.trackIndex))) {
    if (wasPaused) {
      audio.state.paused.store(true);
    }
  }
}

bool audioAdjustVgmOption(AudioPlaybackState& audio, VgmOptionId id,
                          int direction) {
  if (direction == 0) return false;
  bool changed = false;
  switch (id) {
    case VgmOptionId::PlaybackHz: {
      int next = static_cast<int>(audio.vgmOptions.playbackHz) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      audio.vgmOptions.playbackHz = static_cast<VgmPlaybackHz>(next);
      changed = true;
      break;
    }
    case VgmOptionId::Speed: {
      int next = audio.vgmOptions.speedStep + (direction > 0 ? 1 : -1);
      if (next < 0) next = kVgmSpeedStepCount - 1;
      if (next >= kVgmSpeedStepCount) next = 0;
      audio.vgmOptions.speedStep = next;
      changed = true;
      break;
    }
    case VgmOptionId::LoopCount: {
      int idx = findIndex(kVgmLoopSteps,
                          sizeof(kVgmLoopSteps) / sizeof(kVgmLoopSteps[0]),
                          audio.vgmOptions.loopCount);
      idx = advanceIndex(
          idx,
          static_cast<int>(sizeof(kVgmLoopSteps) / sizeof(kVgmLoopSteps[0])),
          direction);
      audio.vgmOptions.loopCount = kVgmLoopSteps[idx];
      changed = true;
      break;
    }
    case VgmOptionId::FadeLength: {
      int idx = findIndex(
          kVgmFadeStepsMs,
          sizeof(kVgmFadeStepsMs) / sizeof(kVgmFadeStepsMs[0]),
          audio.vgmOptions.fadeMs);
      idx = advanceIndex(
          idx,
          static_cast<int>(sizeof(kVgmFadeStepsMs) / sizeof(kVgmFadeStepsMs[0])),
          direction);
      audio.vgmOptions.fadeMs = kVgmFadeStepsMs[idx];
      changed = true;
      break;
    }
    case VgmOptionId::EndSilence: {
      int idx = findIndex(
          kVgmEndSilenceStepsMs,
          sizeof(kVgmEndSilenceStepsMs) / sizeof(kVgmEndSilenceStepsMs[0]),
          audio.vgmOptions.endSilenceMs);
      idx = advanceIndex(
          idx,
          static_cast<int>(sizeof(kVgmEndSilenceStepsMs) /
                           sizeof(kVgmEndSilenceStepsMs[0])),
          direction);
      audio.vgmOptions.endSilenceMs = kVgmEndSilenceStepsMs[idx];
      changed = true;
      break;
    }
    case VgmOptionId::HardStopOld:
      audio.vgmOptions.hardStopOld = !audio.vgmOptions.hardStopOld;
      changed = true;
      break;
    case VgmOptionId::IgnoreVolGain:
      audio.vgmOptions.ignoreVolGain = !audio.vgmOptions.ignoreVolGain;
      changed = true;
      break;
    case VgmOptionId::MasterVolume: {
      int next = audio.vgmOptions.masterVolumeStep +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = kVgmVolumeStepCount - 1;
      if (next >= kVgmVolumeStepCount) next = 0;
      audio.vgmOptions.masterVolumeStep = next;
      changed = true;
      break;
    }
    case VgmOptionId::PhaseInvert: {
      int next = static_cast<int>(audio.vgmOptions.phaseInvert) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 3;
      if (next > 3) next = 0;
      audio.vgmOptions.phaseInvert = static_cast<VgmPhaseInvert>(next);
      changed = true;
      break;
    }
    default:
      break;
  }
  if (changed && isAudioMode(audio, AudioMode::Vgm)) {
    audio.state.vgm.applyOptions(audio.vgmOptions);
    uint64_t totalFrames = 0;
    if (audio.state.vgm.getTotalFrames(&totalFrames)) {
      audio.state.totalFrames.store(totalFrames);
    } else {
      audio.state.totalFrames.store(0);
    }
  }
  return changed;
}

bool audioAdjustVgmDeviceOption(AudioPlaybackState& audio,
                                const VgmDeviceInfo& device,
                                const VgmDeviceOptions& baseline,
                                VgmDeviceOptionId id,
                                int direction) {
  if (direction == 0) return false;

  VgmDeviceOptions options = baseline;
  if (isAudioMode(audio, AudioMode::Vgm)) {
    VgmDeviceOptions activeOptions{};
    if (audio.state.vgm.getDeviceOptions(device.id, &activeOptions)) {
      options = activeOptions;
    }
  } else {
    auto overrideIt = audio.vgmDeviceOverrides.find(device.id);
    if (overrideIt != audio.vgmDeviceOverrides.end()) {
      options = overrideIt->second;
    }
  }

  bool changed = false;
  switch (id) {
    case VgmDeviceOptionId::Mute:
      options.muted = !options.muted;
      changed = true;
      break;
    case VgmDeviceOptionId::Core: {
      std::vector<uint32_t> cores;
      cores.reserve(device.coreIds.size() + 1);
      cores.push_back(0);
      for (uint32_t coreId : device.coreIds) {
        if (coreId != 0) cores.push_back(coreId);
      }
      int idx = 0;
      for (size_t i = 0; i < cores.size(); ++i) {
        if (cores[i] == options.coreId) {
          idx = static_cast<int>(i);
          break;
        }
      }
      idx = advanceIndex(idx, static_cast<int>(cores.size()), direction);
      uint32_t nextCore = cores[idx];
      if (nextCore != options.coreId) {
        options.coreId = nextCore;
        changed = true;
      }
      break;
    }
    case VgmDeviceOptionId::Resampler: {
      int next = static_cast<int>(options.resamplerMode) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      options.resamplerMode = static_cast<uint8_t>(next);
      changed = true;
      break;
    }
    case VgmDeviceOptionId::SampleRateMode: {
      int next = static_cast<int>(options.sampleRateMode) +
                 (direction > 0 ? 1 : -1);
      if (next < 0) next = 2;
      if (next > 2) next = 0;
      options.sampleRateMode = static_cast<uint8_t>(next);
      changed = true;
      break;
    }
    case VgmDeviceOptionId::SampleRate: {
      int idx = findIndex(
          kVgmSampleRateSteps,
          sizeof(kVgmSampleRateSteps) / sizeof(kVgmSampleRateSteps[0]),
          options.sampleRate);
      idx = advanceIndex(
          idx,
          static_cast<int>(sizeof(kVgmSampleRateSteps) /
                           sizeof(kVgmSampleRateSteps[0])),
          direction);
      options.sampleRate = kVgmSampleRateSteps[idx];
      changed = true;
      break;
    }
    default:
      break;
  }

  if (!changed) return false;

  audio.vgmDeviceOverrides[device.id] = options;
  if (isAudioMode(audio, AudioMode::Vgm)) {
    if (id == VgmDeviceOptionId::Mute) {
      audio.state.vgm.setDeviceOptions(device.id, options);
    } else {
      reloadVgmWithOptions(audio);
    }
  }
  return true;
}
