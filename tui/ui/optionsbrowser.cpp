#include "optionsbrowser.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "audioplayback.h"
#include "media_formats.h"
#include "consoleinput.h"
#include "kssoptions.h"
#include "nsfoptions.h"
#include "vgmoptions.h"
#include "ui_helpers.h"

namespace {
enum class OptionsTarget {
  None,
  Kss,
  Nsf,
  Vgm,
};

enum class OptionsBrowserMode {
  Root,
  Instruments,
  VgmDevices,
  VgmDevice,
  VgmMetadata,
};

struct OptionsBrowserState {
  bool active = false;
  OptionsTarget target = OptionsTarget::None;
  OptionsBrowserMode mode = OptionsBrowserMode::Root;
  std::filesystem::path path;
  std::filesystem::path returnDir;
  std::filesystem::path file;
  int trackIndex = 0;
  std::filesystem::path instrumentFile;
  int instrumentTrack = -1;
  std::vector<KssInstrumentProfile> instruments;
  std::filesystem::path vgmMetadataFile;
  std::vector<VgmMetadataEntry> vgmMetadata;
  std::filesystem::path vgmDevicesFile;
  std::vector<VgmDeviceInfo> vgmDevices;
};

OptionsBrowserState gOptionsBrowser;

OptionsTarget targetForPath(const std::filesystem::path& p) {
  if (isKssExt(p)) return OptionsTarget::Kss;
  if (isGmeExt(p)) return OptionsTarget::Nsf;
  if (isVgmExt(p)) return OptionsTarget::Vgm;
  return OptionsTarget::None;
}

std::string qualityLabel(KssQuality quality) {
  switch (quality) {
    case KssQuality::High:
      return "high";
    case KssQuality::Low:
      return "low";
    case KssQuality::Auto:
    default:
      return "auto";
  }
}

std::string sccTypeLabel(KssSccType type) {
  switch (type) {
    case KssSccType::Standard:
      return "standard";
    case KssSccType::Enhanced:
      return "enhanced";
    case KssSccType::Auto:
    default:
      return "auto";
  }
}

std::string psgTypeLabel(KssPsgType type) {
  switch (type) {
    case KssPsgType::Ay:
      return "AY";
    case KssPsgType::Ym:
      return "YM";
    case KssPsgType::Auto:
    default:
      return "auto";
  }
}

std::string opllTypeLabel(KssOpllType type) {
  switch (type) {
    case KssOpllType::Vrc7:
      return "VRC7";
    case KssOpllType::Ymf281b:
      return "YMF281B";
    case KssOpllType::Ym2413:
    default:
      return "YM2413";
  }
}

std::string onOffLabel(bool enabled) {
  return enabled ? "on" : "off";
}

std::string nsfEqLabel(NsfEqPreset preset) {
  return preset == NsfEqPreset::Famicom ? "famicom" : "nes";
}

std::string nsfStereoLabel(NsfStereoDepth depth) {
  switch (depth) {
    case NsfStereoDepth::High:
      return "100%";
    case NsfStereoDepth::Low:
      return "50%";
    case NsfStereoDepth::Off:
    default:
      return "off";
  }
}

std::string nsfTempoLabel(NsfTempoMode mode) {
  return mode == NsfTempoMode::Pal50 ? "50Hz" : "60Hz";
}

std::string vgmPlaybackHzLabel(VgmPlaybackHz mode) {
  switch (mode) {
    case VgmPlaybackHz::Hz50:
      return "50Hz";
    case VgmPlaybackHz::Hz60:
      return "60Hz";
    case VgmPlaybackHz::Auto:
    default:
      return "auto";
  }
}

std::string vgmSpeedLabel(int step) {
  constexpr double kSpeedSteps[] = {0.5, 0.75, 1.0, 1.25, 1.5, 2.0};
  int maxStep =
      static_cast<int>(sizeof(kSpeedSteps) / sizeof(kSpeedSteps[0])) - 1;
  int idx = std::clamp(step, 0, maxStep);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2fx", kSpeedSteps[idx]);
  return std::string(buf);
}

std::string vgmLoopLabel(int loops) {
  if (loops <= 0) return "infinite";
  return std::to_string(loops);
}

std::string vgmMillisLabel(int ms) {
  if (ms <= 0) return "off";
  if (ms % 1000 == 0) {
    return std::to_string(ms / 1000) + "s";
  }
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.1fs", ms / 1000.0);
  return std::string(buf);
}

std::string vgmMasterVolumeLabel(int step) {
  constexpr int kVolumeStepsDb[] = {-12, -6, -3, 0, 3, 6};
  int maxStep =
      static_cast<int>(sizeof(kVolumeStepsDb) / sizeof(kVolumeStepsDb[0])) - 1;
  int idx = std::clamp(step, 0, maxStep);
  int db = kVolumeStepsDb[idx];
  char buf[16];
  if (db >= 0) {
    std::snprintf(buf, sizeof(buf), "+%d dB", db);
  } else {
    std::snprintf(buf, sizeof(buf), "%d dB", db);
  }
  return std::string(buf);
}

std::string vgmPhaseInvertLabel(VgmPhaseInvert mode) {
  switch (mode) {
    case VgmPhaseInvert::Left:
      return "left";
    case VgmPhaseInvert::Right:
      return "right";
    case VgmPhaseInvert::Both:
      return "both";
    case VgmPhaseInvert::Off:
    default:
      return "off";
  }
}

std::string vgmResamplerLabel(uint8_t mode) {
  switch (mode) {
    case 1:
      return "nearest";
    case 2:
      return "mixed";
    case 0:
    default:
      return "linear";
  }
}

std::string vgmSampleRateModeLabel(uint8_t mode) {
  switch (mode) {
    case 1:
      return "custom";
    case 2:
      return "highest";
    case 0:
    default:
      return "native";
  }
}

std::string vgmSampleRateLabel(uint32_t rate) {
  if (rate == 0) return "auto";
  return std::to_string(rate) + " Hz";
}

std::string hex32(uint32_t value) {
  char buf[11];
  std::snprintf(buf, sizeof(buf), "0x%08x", value);
  return std::string(buf);
}

std::filesystem::path instrumentsPath() {
  if (gOptionsBrowser.path.empty()) return {};
  return gOptionsBrowser.path / "Instruments";
}

std::filesystem::path devicesPath() {
  if (gOptionsBrowser.path.empty()) return {};
  return gOptionsBrowser.path / "Devices";
}

std::filesystem::path metadataPath() {
  if (gOptionsBrowser.path.empty()) return {};
  return gOptionsBrowser.path / "Metadata";
}

bool isPathWithin(const std::filesystem::path& base,
                  const std::filesystem::path& path) {
  if (base.empty()) return false;
  auto baseIt = base.begin();
  auto pathIt = path.begin();
  for (; baseIt != base.end(); ++baseIt, ++pathIt) {
    if (pathIt == path.end() || *baseIt != *pathIt) return false;
  }
  return true;
}

bool parseHexId(const std::string& text, uint32_t* out) {
  if (!out) return false;
  if (text.size() < 3 || text[0] != '0' ||
      (text[1] != 'x' && text[1] != 'X')) {
    return false;
  }
  char* end = nullptr;
  unsigned long value = std::strtoul(text.c_str() + 2, &end, 16);
  if (!end || *end != '\0') return false;
  *out = static_cast<uint32_t>(value);
  return true;
}

bool parseDeviceIdFromPath(const std::filesystem::path& path,
                           uint32_t* out) {
  if (!out) return false;
  if (path.parent_path() != devicesPath()) return false;
  return parseHexId(path.filename().string(), out);
}

bool isOptionsPath(const std::filesystem::path& path) {
  if (!gOptionsBrowser.active) return false;
  return isPathWithin(gOptionsBrowser.path, path);
}

OptionsBrowserMode modeForPath(const std::filesystem::path& path) {
  if (path == instrumentsPath()) return OptionsBrowserMode::Instruments;
  if (path == metadataPath()) return OptionsBrowserMode::VgmMetadata;
  if (path == devicesPath()) return OptionsBrowserMode::VgmDevices;
  uint32_t deviceId = 0;
  if (parseDeviceIdFromPath(path, &deviceId)) {
    return OptionsBrowserMode::VgmDevice;
  }
  return OptionsBrowserMode::Root;
}

std::filesystem::path resolveOptionsFile(const BrowserState& browser,
                                         OptionsTarget* targetOut) {
  if (targetOut) *targetOut = OptionsTarget::None;
  if (!browser.entries.empty()) {
    int idx = std::clamp(browser.selected, 0,
                         static_cast<int>(browser.entries.size()) - 1);
    const auto& entry = browser.entries[static_cast<size_t>(idx)];
    if (!entry.isDir) {
      OptionsTarget target = targetForPath(entry.path);
      if (target != OptionsTarget::None) {
        if (targetOut) *targetOut = target;
        return entry.path;
      }
    }
  }
  std::filesystem::path nowPlaying = audioGetNowPlaying();
  OptionsTarget target = targetForPath(nowPlaying);
  if (target != OptionsTarget::None) {
    if (targetOut) *targetOut = target;
    return nowPlaying;
  }
  return {};
}

void buildOptionsEntries(std::vector<FileEntry>& entries) {
  entries.clear();
  if (!gOptionsBrowser.returnDir.empty()) {
    entries.push_back(FileEntry{"..", gOptionsBrowser.returnDir, true});
  }

  if (gOptionsBrowser.target == OptionsTarget::Kss) {
    KssPlaybackOptions options = audioGetKssOptionState();
    auto addOption = [&](KssOptionId id, const std::string& label) {
      FileEntry entry;
      entry.name = label;
      entry.path = gOptionsBrowser.path;
      entry.isDir = false;
      entry.optionId = static_cast<int>(id);
      entries.push_back(std::move(entry));
    };

    addOption(KssOptionId::Force50Hz,
              "50Hz: " + std::string(options.force50Hz ? "forced" : "auto"));
    addOption(KssOptionId::PsgType,
              "PSG chip: " + psgTypeLabel(options.psgType));
    addOption(KssOptionId::SccType,
              "SCC type: " + sccTypeLabel(options.sccType));
    addOption(KssOptionId::OpllType,
              "OPLL chip: " + opllTypeLabel(options.opllType));
    addOption(KssOptionId::PsgQuality,
              "PSG quality: " + qualityLabel(options.psgQuality));
    addOption(KssOptionId::SccQuality,
              "SCC quality: " + qualityLabel(options.sccQuality));
    addOption(KssOptionId::OpllStereo,
              "OPLL stereo: " + onOffLabel(options.opllStereo));
    addOption(KssOptionId::MutePsg,
              "PSG mute: " + onOffLabel(options.mutePsg));
    addOption(KssOptionId::MuteScc,
              "SCC mute: " + onOffLabel(options.muteScc));
    addOption(KssOptionId::MuteOpll,
              "OPLL mute: " + onOffLabel(options.muteOpll));

    FileEntry instruments;
    instruments.name = "Instrument list";
    instruments.path = instrumentsPath();
    instruments.isDir = true;
    entries.push_back(std::move(instruments));
  } else if (gOptionsBrowser.target == OptionsTarget::Nsf) {
    NsfPlaybackOptions options = audioGetNsfOptionState();
    auto addOption = [&](NsfOptionId id, const std::string& label) {
      FileEntry entry;
      entry.name = label;
      entry.path = gOptionsBrowser.path;
      entry.isDir = false;
      entry.optionId = static_cast<int>(id);
      entries.push_back(std::move(entry));
    };

    addOption(NsfOptionId::EqPreset,
              "EQ: " + nsfEqLabel(options.eqPreset));
    addOption(NsfOptionId::StereoDepth,
              "Stereo depth: " + nsfStereoLabel(options.stereoDepth));
    addOption(NsfOptionId::IgnoreSilence,
              "Ignore silence: " + onOffLabel(options.ignoreSilence));
    addOption(NsfOptionId::TempoMode,
              "Speed: " + nsfTempoLabel(options.tempoMode));
  } else if (gOptionsBrowser.target == OptionsTarget::Vgm) {
    VgmPlaybackOptions options = audioGetVgmOptionState();
    auto addOption = [&](VgmOptionId id, const std::string& label) {
      FileEntry entry;
      entry.name = label;
      entry.path = gOptionsBrowser.path;
      entry.isDir = false;
      entry.optionId = static_cast<int>(id);
      entries.push_back(std::move(entry));
    };

    addOption(VgmOptionId::PlaybackHz,
              "Playback Hz: " + vgmPlaybackHzLabel(options.playbackHz));
    addOption(VgmOptionId::Speed, "Speed: " + vgmSpeedLabel(options.speedStep));
    addOption(VgmOptionId::LoopCount,
              "Loop count: " + vgmLoopLabel(options.loopCount));
    addOption(VgmOptionId::FadeLength,
              "Fade: " + vgmMillisLabel(options.fadeMs));
    addOption(VgmOptionId::EndSilence,
              "End silence: " + vgmMillisLabel(options.endSilenceMs));
    addOption(VgmOptionId::HardStopOld,
              "Hard stop old: " + onOffLabel(options.hardStopOld));
    addOption(VgmOptionId::IgnoreVolGain,
              "Ignore vol gain: " + onOffLabel(options.ignoreVolGain));
    addOption(VgmOptionId::MasterVolume,
              "Master volume: " + vgmMasterVolumeLabel(options.masterVolumeStep));
    addOption(VgmOptionId::PhaseInvert,
              "Phase invert: " + vgmPhaseInvertLabel(options.phaseInvert));

    FileEntry devices;
    devices.name = "Devices";
    devices.path = devicesPath();
    devices.isDir = true;
    entries.push_back(std::move(devices));

    FileEntry metadata;
    metadata.name = "Metadata";
    metadata.path = metadataPath();
    metadata.isDir = true;
    entries.push_back(std::move(metadata));
  }
}

void buildInstrumentEntries(std::vector<FileEntry>& entries) {
  entries.clear();
  entries.push_back(FileEntry{"..", gOptionsBrowser.path, true});

  KssInstrumentDevice auditionDevice = KssInstrumentDevice::None;
  uint32_t auditionHash = 0;
  bool auditionActive =
      audioGetKssInstrumentAuditionState(&auditionDevice, &auditionHash);
  if (auditionActive && auditionDevice != KssInstrumentDevice::None) {
    std::string auditionLabel = "Audition: stop";
    if (auditionDevice == KssInstrumentDevice::Psg) {
      auditionLabel += " (PSG " + hex32(auditionHash) + ")";
    } else if (auditionDevice == KssInstrumentDevice::Scc) {
      auditionLabel += " (SCC " + hex32(auditionHash) + ")";
    }
    FileEntry auditionStop;
    auditionStop.name = auditionLabel;
    auditionStop.path = gOptionsBrowser.path;
    auditionStop.isDir = false;
    auditionStop.auditionDevice = static_cast<int>(KssInstrumentDevice::None);
    entries.push_back(std::move(auditionStop));
  }

  if (gOptionsBrowser.target != OptionsTarget::Kss) return;

  int trackIndex = gOptionsBrowser.trackIndex;
  if (!auditionActive) {
    std::filesystem::path nowPlaying = audioGetNowPlaying();
    if (!nowPlaying.empty() && nowPlaying == gOptionsBrowser.file) {
      int currentTrack = audioGetTrackIndex();
      if (currentTrack >= 0) {
        trackIndex = currentTrack;
        gOptionsBrowser.trackIndex = currentTrack;
      }
    }
  }

  if (gOptionsBrowser.instrumentFile != gOptionsBrowser.file ||
      gOptionsBrowser.instrumentTrack != trackIndex) {
    gOptionsBrowser.instruments.clear();
    std::string error;
    bool ok = audioScanKssInstruments(gOptionsBrowser.file, trackIndex,
                                      &gOptionsBrowser.instruments, &error);
    gOptionsBrowser.instrumentFile = gOptionsBrowser.file;
    gOptionsBrowser.instrumentTrack = trackIndex;
    if (!ok && !error.empty()) {
      FileEntry entry;
      entry.name = "Scan failed: " + error;
      entry.path = gOptionsBrowser.path;
      entry.isDir = false;
      entries.push_back(std::move(entry));
      return;
    }
  }

  for (size_t i = 0; i < gOptionsBrowser.instruments.size(); ++i) {
    const auto& instrument = gOptionsBrowser.instruments[i];
    std::string label;
    if (instrument.device == KssInstrumentDevice::Psg) {
      label = "PSG inst " + hex32(instrument.hash);
    } else if (instrument.device == KssInstrumentDevice::Scc) {
      label = "SCC wave " + hex32(instrument.hash);
    } else {
      continue;
    }

    FileEntry entry;
    entry.name = label;
    entry.path = gOptionsBrowser.path;
    entry.isDir = false;
    entry.auditionIndex = static_cast<int>(i);
    entries.push_back(std::move(entry));
  }

  if (gOptionsBrowser.instruments.empty()) {
    FileEntry entry;
    entry.name = "(no instruments found)";
    entry.path = gOptionsBrowser.path;
    entry.isDir = false;
    entries.push_back(std::move(entry));
  }
}

void buildVgmMetadataEntries(std::vector<FileEntry>& entries) {
  entries.clear();
  entries.push_back(FileEntry{"..", gOptionsBrowser.path, true});

  if (gOptionsBrowser.target != OptionsTarget::Vgm) return;

  if (gOptionsBrowser.vgmMetadataFile != gOptionsBrowser.file) {
    gOptionsBrowser.vgmMetadata.clear();
    std::string error;
    bool ok = audioScanVgmMetadata(gOptionsBrowser.file,
                                   &gOptionsBrowser.vgmMetadata, &error);
    gOptionsBrowser.vgmMetadataFile = gOptionsBrowser.file;
    if (!ok && !error.empty()) {
      FileEntry entry;
      entry.name = "Scan failed: " + error;
      entry.path = metadataPath();
      entry.isDir = false;
      entries.push_back(std::move(entry));
      return;
    }
  }

  for (const auto& meta : gOptionsBrowser.vgmMetadata) {
    FileEntry entry;
    entry.name = meta.key + ": " + meta.value;
    entry.path = metadataPath();
    entry.isDir = false;
    entries.push_back(std::move(entry));
  }

  if (gOptionsBrowser.vgmMetadata.empty()) {
    FileEntry entry;
    entry.name = "(no metadata)";
    entry.path = metadataPath();
    entry.isDir = false;
    entries.push_back(std::move(entry));
  }
}

void buildVgmDeviceEntries(std::vector<FileEntry>& entries) {
  entries.clear();
  entries.push_back(FileEntry{"..", gOptionsBrowser.path, true});

  if (gOptionsBrowser.target != OptionsTarget::Vgm) return;

  if (gOptionsBrowser.vgmDevicesFile != gOptionsBrowser.file) {
    gOptionsBrowser.vgmDevices.clear();
    std::string error;
    bool ok =
        audioScanVgmDevices(gOptionsBrowser.file, &gOptionsBrowser.vgmDevices,
                            &error);
    gOptionsBrowser.vgmDevicesFile = gOptionsBrowser.file;
    if (!ok && !error.empty()) {
      FileEntry entry;
      entry.name = "Scan failed: " + error;
      entry.path = devicesPath();
      entry.isDir = false;
      entries.push_back(std::move(entry));
      return;
    }
  }

  for (const auto& device : gOptionsBrowser.vgmDevices) {
    std::string label = device.name;
    if (device.channelCount > 0) {
      label += " (" + std::to_string(device.channelCount) + " ch)";
    }
    FileEntry entry;
    entry.name = label;
    entry.path = devicesPath() / hex32(device.id);
    entry.isDir = true;
    entries.push_back(std::move(entry));
  }

  if (gOptionsBrowser.vgmDevices.empty()) {
    FileEntry entry;
    entry.name = "(no devices found)";
    entry.path = devicesPath();
    entry.isDir = false;
    entries.push_back(std::move(entry));
  }
}

void buildVgmDeviceOptionEntries(std::vector<FileEntry>& entries,
                                 uint32_t deviceId) {
  entries.clear();
  entries.push_back(FileEntry{"..", devicesPath(), true});

  if (gOptionsBrowser.target != OptionsTarget::Vgm) return;

  VgmDeviceOptions options{};
  if (!audioGetVgmDeviceOptions(deviceId, &options)) {
    FileEntry entry;
    entry.name = "Device options unavailable";
    entry.path = devicesPath();
    entry.isDir = false;
    entries.push_back(std::move(entry));
    return;
  }

  const VgmDeviceInfo* deviceInfo = nullptr;
  for (const auto& device : gOptionsBrowser.vgmDevices) {
    if (device.id == deviceId) {
      deviceInfo = &device;
      break;
    }
  }

  std::string coreLabel = "auto";
  if (options.coreId != 0 && deviceInfo) {
    for (size_t i = 0; i < deviceInfo->coreIds.size(); ++i) {
      if (deviceInfo->coreIds[i] == options.coreId &&
          i < deviceInfo->coreNames.size()) {
        coreLabel = deviceInfo->coreNames[i];
        break;
      }
    }
  }
  if (options.coreId != 0 && coreLabel == "auto") {
    coreLabel = hex32(options.coreId);
  }

  auto addOption = [&](VgmDeviceOptionId id, const std::string& label) {
    FileEntry entry;
    entry.name = label;
    entry.path = gOptionsBrowser.path;
    entry.isDir = false;
    entry.optionId = static_cast<int>(id);
    entries.push_back(std::move(entry));
  };

  addOption(VgmDeviceOptionId::Mute,
            "Mute: " + onOffLabel(options.muted));
  addOption(VgmDeviceOptionId::Core, "Core: " + coreLabel);
  addOption(VgmDeviceOptionId::Resampler,
            "Resampler: " + vgmResamplerLabel(options.resamplerMode));
  addOption(VgmDeviceOptionId::SampleRateMode,
            "Sample rate mode: " +
                vgmSampleRateModeLabel(options.sampleRateMode));
  addOption(VgmDeviceOptionId::SampleRate,
            "Sample rate: " + vgmSampleRateLabel(options.sampleRate));
}
}  // namespace

bool optionsBrowserIsActive(const BrowserState& browser) {
  return isOptionsPath(browser.dir);
}

bool optionsBrowserCanToggle(const BrowserState& browser) {
  if (optionsBrowserIsActive(browser)) return true;
  OptionsTarget target = OptionsTarget::None;
  return !resolveOptionsFile(browser, &target).empty();
}

bool optionsBrowserRefresh(BrowserState& browser) {
  if (!isOptionsPath(browser.dir)) {
    if (gOptionsBrowser.active && !isOptionsPath(browser.dir)) {
      gOptionsBrowser.active = false;
      gOptionsBrowser.target = OptionsTarget::None;
      gOptionsBrowser.mode = OptionsBrowserMode::Root;
    }
    return false;
  }
  gOptionsBrowser.mode = modeForPath(browser.dir);
  if (gOptionsBrowser.mode == OptionsBrowserMode::Instruments) {
    buildInstrumentEntries(browser.entries);
  } else if (gOptionsBrowser.mode == OptionsBrowserMode::VgmMetadata) {
    buildVgmMetadataEntries(browser.entries);
  } else if (gOptionsBrowser.mode == OptionsBrowserMode::VgmDevices) {
    buildVgmDeviceEntries(browser.entries);
  } else if (gOptionsBrowser.mode == OptionsBrowserMode::VgmDevice) {
    uint32_t deviceId = 0;
    if (parseDeviceIdFromPath(browser.dir, &deviceId)) {
      buildVgmDeviceOptionEntries(browser.entries, deviceId);
    } else {
      buildOptionsEntries(browser.entries);
    }
  } else {
    buildOptionsEntries(browser.entries);
  }
  return true;
}

OptionsBrowserResult optionsBrowserActivateSelection(BrowserState& browser) {
  if (!optionsBrowserIsActive(browser)) {
    return OptionsBrowserResult::NotHandled;
  }
  if (browser.entries.empty()) {
    return OptionsBrowserResult::Handled;
  }
  int idx = std::clamp(browser.selected, 0,
                       static_cast<int>(browser.entries.size()) - 1);
  const auto& entry = browser.entries[static_cast<size_t>(idx)];
  if (gOptionsBrowser.mode == OptionsBrowserMode::Instruments) {
    if (entry.auditionDevice >= 0) {
      auto device =
          static_cast<KssInstrumentDevice>(entry.auditionDevice);
      if (device == KssInstrumentDevice::None &&
          audioStopKssInstrumentAudition()) {
        return OptionsBrowserResult::Changed;
      }
    }
    if (entry.auditionIndex >= 0 &&
        entry.auditionIndex <
            static_cast<int>(gOptionsBrowser.instruments.size())) {
      if (audioStartKssInstrumentAudition(
              gOptionsBrowser.instruments[entry.auditionIndex])) {
        return OptionsBrowserResult::Changed;
      }
    }
    return OptionsBrowserResult::Handled;
  } else if (gOptionsBrowser.mode == OptionsBrowserMode::VgmDevice) {
    uint32_t deviceId = 0;
    if (entry.optionId >= 0 && parseDeviceIdFromPath(browser.dir, &deviceId)) {
      if (audioAdjustVgmDeviceOption(
              deviceId, static_cast<VgmDeviceOptionId>(entry.optionId))) {
        return OptionsBrowserResult::Changed;
      }
    }
    return OptionsBrowserResult::Handled;
  }
  if (entry.optionId >= 0) {
    if (gOptionsBrowser.target == OptionsTarget::Kss) {
      if (audioAdjustKssOption(static_cast<KssOptionId>(entry.optionId))) {
        return OptionsBrowserResult::Changed;
      }
    } else if (gOptionsBrowser.target == OptionsTarget::Nsf) {
      if (audioAdjustNsfOption(static_cast<NsfOptionId>(entry.optionId))) {
        return OptionsBrowserResult::Changed;
      }
    } else if (gOptionsBrowser.target == OptionsTarget::Vgm) {
      if (audioAdjustVgmOption(static_cast<VgmOptionId>(entry.optionId))) {
        return OptionsBrowserResult::Changed;
      }
    }
  }
  return OptionsBrowserResult::Handled;
}

void optionsBrowserToggle(BrowserState& browser) {
  if (optionsBrowserIsActive(browser)) {
    browser.dir = gOptionsBrowser.returnDir;
    gOptionsBrowser.active = false;
    gOptionsBrowser.target = OptionsTarget::None;
    gOptionsBrowser.mode = OptionsBrowserMode::Root;
    return;
  }

  OptionsTarget target = OptionsTarget::None;
  std::filesystem::path file = resolveOptionsFile(browser, &target);
  if (file.empty() || target == OptionsTarget::None) {
    return;
  }
  gOptionsBrowser.active = true;
  gOptionsBrowser.target = target;
  gOptionsBrowser.mode = OptionsBrowserMode::Root;
  gOptionsBrowser.returnDir = browser.dir;
  gOptionsBrowser.file = file;
  gOptionsBrowser.trackIndex = 0;
  if (!browser.entries.empty()) {
    int idx = std::clamp(browser.selected, 0,
                         static_cast<int>(browser.entries.size()) - 1);
    const auto& entry = browser.entries[static_cast<size_t>(idx)];
    if (!entry.isDir && entry.trackIndex >= 0) {
      gOptionsBrowser.trackIndex = entry.trackIndex;
    }
  }
  std::filesystem::path nowPlaying = audioGetNowPlaying();
  if (!nowPlaying.empty() && nowPlaying == gOptionsBrowser.file) {
    int currentTrack = audioGetTrackIndex();
    if (currentTrack >= 0) {
      gOptionsBrowser.trackIndex = currentTrack;
    }
  }
  gOptionsBrowser.path = browser.dir / "Options";
  browser.dir = gOptionsBrowser.path;
  browser.selected = 0;
  browser.scrollRow = 0;
  browser.filter.clear();
  browser.filterActive = false;
}

bool optionsBrowserNavigateUp(BrowserState& browser) {
  if (!optionsBrowserIsActive(browser)) return false;
  if (gOptionsBrowser.mode == OptionsBrowserMode::Instruments) {
    browser.dir = gOptionsBrowser.path;
    gOptionsBrowser.mode = OptionsBrowserMode::Root;
  } else if (gOptionsBrowser.mode == OptionsBrowserMode::VgmDevice) {
    browser.dir = devicesPath();
    gOptionsBrowser.mode = OptionsBrowserMode::VgmDevices;
  } else if (gOptionsBrowser.mode == OptionsBrowserMode::VgmDevices ||
             gOptionsBrowser.mode == OptionsBrowserMode::VgmMetadata) {
    browser.dir = gOptionsBrowser.path;
    gOptionsBrowser.mode = OptionsBrowserMode::Root;
  } else {
    if (gOptionsBrowser.returnDir.empty()) return false;
    browser.dir = gOptionsBrowser.returnDir;
    gOptionsBrowser.active = false;
    gOptionsBrowser.target = OptionsTarget::None;
    gOptionsBrowser.mode = OptionsBrowserMode::Root;
  }
  browser.selected = 0;
  browser.scrollRow = 0;
  browser.filter.clear();
  browser.filterActive = false;
  return true;
}

std::string optionsBrowserSelectionMeta(const BrowserState& browser) {
  if (browser.entries.empty()) return "";
  int idx = std::clamp(browser.selected, 0,
                       static_cast<int>(browser.entries.size()) - 1);
  const auto& entry = browser.entries[static_cast<size_t>(idx)];
  std::string name = entry.name;
  if (entry.isDir && name != "..") name += "/";

  std::string sortLabel = "Name";
  if (browser.sortMode == BrowserState::SortMode::Date) sortLabel = "Date";
  else if (browser.sortMode == BrowserState::SortMode::Size) sortLabel = "Size";

  std::string dirArrow = browser.sortDescending ? " \xE2\x86\x93" : " \xE2\x86\x91";
  std::string metaLine = " [" + sortLabel + dirArrow + "]";

  metaLine += " Selected: " + name;
  return metaLine;
}

std::string optionsBrowserShowingLabel() {
  std::string label = "  Showing: ";
  if (gOptionsBrowser.mode == OptionsBrowserMode::Instruments) {
    label += "instruments";
  } else if (gOptionsBrowser.mode == OptionsBrowserMode::VgmDevices) {
    label += "devices";
  } else if (gOptionsBrowser.mode == OptionsBrowserMode::VgmDevice) {
    label += "device options";
  } else if (gOptionsBrowser.mode == OptionsBrowserMode::VgmMetadata) {
    label += "metadata";
  } else {
    label += "options";
  }
  if (!gOptionsBrowser.file.empty()) {
    label += " for " + toUtf8String(gOptionsBrowser.file.filename());
  }
  return label;
}
