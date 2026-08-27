#include "optionsbrowser.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "audioplayback.h"
#include "media_formats.h"
#include "kssoptions.h"
#include "nsfoptions.h"
#include "runtime_helpers.h"
#include "vgmoptions.h"
#include "ui_helpers.h"

struct OptionsBrowserContent {
  bool instrumentsLoaded = false;
  std::filesystem::path instrumentFile;
  std::optional<uint32_t> instrumentTrack;
  std::string instrumentError;
  std::vector<KssInstrumentProfile> instruments;

  bool vgmMetadataLoaded = false;
  std::filesystem::path vgmMetadataFile;
  std::string vgmMetadataError;
  std::vector<VgmMetadataEntry> vgmMetadata;

  bool vgmDevicesLoaded = false;
  std::filesystem::path vgmDevicesFile;
  std::string vgmDevicesError;
  std::vector<VgmDeviceInfo> vgmDevices;
  std::unordered_map<uint32_t, VgmDeviceOptions> vgmDeviceDefaults;
};

namespace {
enum class OptionsTarget {
  None,
  Kss,
  Nsf,
  Vgm,
};

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

BrowserLocation optionsPageLocation(const BrowserLocation& location,
                                    BrowserOptionsPage page) {
  return browserOptionsLocation(location.path(),
                                browserOptionsTrackIndex(location),
                                std::move(page));
}

BrowserEntry optionsDirectoryEntry(const std::string& name,
                                   const BrowserLocation& target) {
  return {name, {}, browser_entry::OpenLocation{target}};
}

BrowserEntry optionsParentEntry() {
  return {"..", {}, browser_entry::NavigateUp{}};
}

void buildOptionsEntries(std::vector<BrowserEntry>& entries,
                         const BrowserLocation& location,
                         const OptionsBrowserRuntimeSnapshot& runtime) {
  entries.clear();
  entries.emplace_back("..", std::filesystem::path{},
                       browser_entry::NavigateUp{});

  const OptionsTarget target = targetForPath(location.path());
  if (target == OptionsTarget::Kss) {
    const KssPlaybackOptions& options = runtime.kssOptions;
    auto addOption = [&](KssOptionId id, const std::string& label) {
      entries.emplace_back(label, std::filesystem::path{},
                           browser_entry::AdjustKssOption{id});
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

    if (const std::optional<uint32_t> trackIndex =
            browserOptionsTrackIndex(location)) {
      entries.push_back(optionsDirectoryEntry(
          "Instrument list",
          optionsPageLocation(location,
                              BrowserOptionsInstruments{*trackIndex})));
    }
  } else if (target == OptionsTarget::Nsf) {
    const NsfPlaybackOptions& options = runtime.nsfOptions;
    auto addOption = [&](NsfOptionId id, const std::string& label) {
      entries.emplace_back(label, std::filesystem::path{},
                           browser_entry::AdjustNsfOption{id});
    };

    addOption(NsfOptionId::EqPreset,
              "EQ: " + nsfEqLabel(options.eqPreset));
    addOption(NsfOptionId::StereoDepth,
              "Stereo depth: " + nsfStereoLabel(options.stereoDepth));
    addOption(NsfOptionId::IgnoreSilence,
              "Ignore silence: " + onOffLabel(options.ignoreSilence));
    addOption(NsfOptionId::TempoMode,
              "Speed: " + nsfTempoLabel(options.tempoMode));
  } else if (target == OptionsTarget::Vgm) {
    const VgmPlaybackOptions& options = runtime.vgmOptions;
    auto addOption = [&](VgmOptionId id, const std::string& label) {
      entries.emplace_back(label, std::filesystem::path{},
                           browser_entry::AdjustVgmOption{id});
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

    entries.push_back(optionsDirectoryEntry(
        "Devices",
        optionsPageLocation(location, BrowserOptionsVgmDevices{})));
    entries.push_back(optionsDirectoryEntry(
        "Metadata",
        optionsPageLocation(location, BrowserOptionsVgmMetadata{})));
  }
}

void buildInstrumentEntries(std::vector<BrowserEntry>& entries,
                            const BrowserLocation& location,
                            OptionsBrowserContent& content,
                            const OptionsBrowserRuntimeSnapshot& runtime,
                            const std::function<bool()>& cancellationRequested) {
  entries.clear();
  entries.push_back(optionsParentEntry());

  const KssInstrumentDevice auditionDevice = runtime.auditionDevice;
  const uint32_t auditionHash = runtime.auditionHash;
  const bool auditionActive = runtime.instrumentAuditionActive;
  if (auditionActive && auditionDevice != KssInstrumentDevice::None) {
    std::string auditionLabel = "Audition: stop";
    if (auditionDevice == KssInstrumentDevice::Psg) {
      auditionLabel += " (PSG " + hex32(auditionHash) + ")";
    } else if (auditionDevice == KssInstrumentDevice::Scc) {
      auditionLabel += " (SCC " + hex32(auditionHash) + ")";
    }
    entries.emplace_back(auditionLabel, std::filesystem::path{},
                         browser_entry::StopInstrumentAudition{});
  }

  if (targetForPath(location.path()) != OptionsTarget::Kss) return;

  const std::optional<uint32_t> routeTrackIndex =
      browserOptionsInstrumentTrackIndex(location);
  if (!routeTrackIndex) {
    entries.emplace_back("Instrument scan unavailable: no track selected",
                         std::filesystem::path{}, browser_entry::Status{});
    return;
  }
  const int trackIndex = static_cast<int>(*routeTrackIndex);

  if (!content.instrumentsLoaded ||
      !samePath(content.instrumentFile, location.path()) ||
      content.instrumentTrack != routeTrackIndex) {
    content.instruments.clear();
    content.instrumentError.clear();
    std::string error;
    bool ok = audioScanKssInstruments(location.path(), trackIndex,
                                      runtime.scanSampleRate,
                                      runtime.kssOptions,
                                      cancellationRequested,
                                      &content.instruments, &error);
    content.instrumentsLoaded = true;
    content.instrumentFile = location.path();
    content.instrumentTrack = routeTrackIndex;
    if (!ok) {
      content.instrumentError =
          error.empty() ? "Unable to scan instruments" : std::move(error);
    }
  }

  if (!content.instrumentError.empty()) {
    entries.emplace_back("Scan failed: " + content.instrumentError,
                         std::filesystem::path{}, browser_entry::Status{});
    return;
  }

  for (size_t i = 0; i < content.instruments.size(); ++i) {
    const auto& instrument = content.instruments[i];
    std::string label;
    if (instrument.device == KssInstrumentDevice::Psg) {
      label = "PSG inst " + hex32(instrument.hash);
    } else if (instrument.device == KssInstrumentDevice::Scc) {
      label = "SCC wave " + hex32(instrument.hash);
    } else {
      continue;
    }

    entries.emplace_back(label, std::filesystem::path{},
                         browser_entry::StartInstrumentAudition{i});
  }

  if (content.instruments.empty()) {
    entries.emplace_back("(no instruments found)", std::filesystem::path{},
                         browser_entry::Status{});
  }
}

void buildVgmMetadataEntries(std::vector<BrowserEntry>& entries,
                             const BrowserLocation& location,
                             OptionsBrowserContent& content,
                             const std::function<bool()>& cancellationRequested) {
  entries.clear();
  entries.push_back(optionsParentEntry());

  if (targetForPath(location.path()) != OptionsTarget::Vgm) return;

  if (!content.vgmMetadataLoaded ||
      !samePath(content.vgmMetadataFile, location.path())) {
    if (cancellationRequested && cancellationRequested()) {
      return;
    }
    content.vgmMetadata.clear();
    content.vgmMetadataError.clear();
    std::string error;
    bool ok = audioScanVgmMetadata(location.path(),
                                   &content.vgmMetadata, &error);
    if (cancellationRequested && cancellationRequested()) {
      return;
    }
    content.vgmMetadataLoaded = true;
    content.vgmMetadataFile = location.path();
    if (!ok) {
      content.vgmMetadataError =
          error.empty() ? "Unable to scan metadata" : std::move(error);
    }
  }

  if (!content.vgmMetadataError.empty()) {
    entries.emplace_back("Scan failed: " + content.vgmMetadataError,
                         std::filesystem::path{}, browser_entry::Status{});
    return;
  }

  for (const auto& meta : content.vgmMetadata) {
    entries.emplace_back(meta.key + ": " + meta.value,
                         std::filesystem::path{},
                         browser_entry::Information{});
  }

  if (content.vgmMetadata.empty()) {
    entries.emplace_back("(no metadata)", std::filesystem::path{},
                         browser_entry::Status{});
  }
}

void loadVgmDevices(const std::filesystem::path& file,
                    OptionsBrowserContent& content,
                    const OptionsBrowserRuntimeSnapshot& runtime,
                    const std::function<bool()>& cancellationRequested) {
  if (content.vgmDevicesLoaded &&
      samePath(content.vgmDevicesFile, file)) {
    return;
  }

  content.vgmDevices.clear();
  content.vgmDeviceDefaults.clear();
  content.vgmDevicesError.clear();
  if (cancellationRequested && cancellationRequested()) {
    return;
  }
  std::string error;
  VgmDeviceCatalog catalog;
  const bool ok = audioScanVgmDevices(file, runtime.scanChannels,
                                      runtime.scanSampleRate, &catalog,
                                      &error);
  if (cancellationRequested && cancellationRequested()) {
    return;
  }
  content.vgmDevices = std::move(catalog.devices);
  content.vgmDeviceDefaults = std::move(catalog.defaults);
  content.vgmDevicesLoaded = true;
  content.vgmDevicesFile = file;
  if (!ok) {
    content.vgmDevicesError =
        error.empty() ? "Unable to scan devices" : std::move(error);
  }
}

void buildVgmDeviceEntries(std::vector<BrowserEntry>& entries,
                           const BrowserLocation& location,
                           OptionsBrowserContent& content,
                           const OptionsBrowserRuntimeSnapshot& runtime,
                           const std::function<bool()>& cancellationRequested) {
  entries.clear();
  entries.push_back(optionsParentEntry());

  if (targetForPath(location.path()) != OptionsTarget::Vgm) return;

  loadVgmDevices(location.path(), content, runtime, cancellationRequested);

  if (!content.vgmDevicesError.empty()) {
    entries.emplace_back("Scan failed: " + content.vgmDevicesError,
                         std::filesystem::path{}, browser_entry::Status{});
    return;
  }

  for (const auto& device : content.vgmDevices) {
    std::string label = device.name;
    if (device.channelCount > 0) {
      label += " (" + std::to_string(device.channelCount) + " ch)";
    }
    entries.push_back(optionsDirectoryEntry(
        label, optionsPageLocation(
                   location, BrowserOptionsVgmDevice{device.id})));
  }

  if (content.vgmDevices.empty()) {
    entries.emplace_back("(no devices found)", std::filesystem::path{},
                         browser_entry::Status{});
  }
}

void buildVgmDeviceOptionEntries(std::vector<BrowserEntry>& entries,
                                 const BrowserLocation& location,
                                 OptionsBrowserContent& content,
                                 const OptionsBrowserRuntimeSnapshot& runtime,
                                 const std::function<bool()>& cancellationRequested) {
  entries.clear();
  entries.push_back(optionsParentEntry());

  if (targetForPath(location.path()) != OptionsTarget::Vgm) return;

  const std::optional<uint32_t> deviceId = browserOptionsDeviceId(location);
  if (!deviceId) return;
  loadVgmDevices(location.path(), content, runtime, cancellationRequested);

  if (!content.vgmDevicesError.empty()) {
    entries.emplace_back("Scan failed: " + content.vgmDevicesError,
                         std::filesystem::path{}, browser_entry::Status{});
    return;
  }

  const auto defaultOptions = content.vgmDeviceDefaults.find(*deviceId);
  if (defaultOptions == content.vgmDeviceDefaults.end()) {
    entries.emplace_back("Device options unavailable",
                         std::filesystem::path{}, browser_entry::Status{});
    return;
  }
  VgmDeviceOptions options = runtime.selectedVgmDeviceOptions.value_or(
      defaultOptions->second);

  const VgmDeviceInfo* deviceInfo = nullptr;
  for (const auto& device : content.vgmDevices) {
    if (device.id == *deviceId) {
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
    entries.emplace_back(label, std::filesystem::path{},
                         browser_entry::AdjustVgmDeviceOption{id});
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
  return browser.location.kind() == BrowserLocationKind::OptionsBrowser;
}

bool optionsBrowserSupportsLocation(const BrowserLocation& location) {
  if (location.kind() != BrowserLocationKind::OptionsBrowser) {
    return false;
  }
  const OptionsTarget target = targetForPath(location.path());
  if (target == OptionsTarget::None) {
    return false;
  }
  switch (browserOptionsPageKind(location)) {
    case BrowserOptionsPageKind::Root:
      return true;
    case BrowserOptionsPageKind::Instruments:
      return target == OptionsTarget::Kss;
    case BrowserOptionsPageKind::VgmDevices:
    case BrowserOptionsPageKind::VgmDevice:
    case BrowserOptionsPageKind::VgmMetadata:
      return target == OptionsTarget::Vgm;
  }
  return false;
}

OptionsBrowserRuntimeSnapshot captureOptionsBrowserRuntimeSnapshot(
    const BrowserLocation& location, const AudioPlaybackRuntime& audioPlayback,
    uint32_t sampleRate, uint32_t channels) {
  OptionsBrowserRuntimeSnapshot runtime;
  runtime.kssOptions = audioPlayback.kssOptions();
  runtime.nsfOptions = audioPlayback.nsfOptions();
  runtime.vgmOptions = audioPlayback.vgmOptions();
  runtime.scanSampleRate = sampleRate == 0 ? 48000 : sampleRate;
  runtime.scanChannels = channels == 0 ? 2 : channels;
  runtime.instrumentAuditionActive = audioPlayback.kssInstrumentAuditionState(
      &runtime.auditionDevice, &runtime.auditionHash);
  if (browserOptionsPageKind(location) ==
      BrowserOptionsPageKind::VgmDevice) {
    const std::optional<uint32_t> deviceId = browserOptionsDeviceId(location);
    if (!deviceId) return runtime;
    VgmDeviceOptions options;
    if (audioPlayback.vgmDeviceOptions(*deviceId, &options)) {
      runtime.selectedVgmDeviceOptions = options;
    }
  }
  return runtime;
}

static const OptionsBrowserContent* optionsBrowserContent(
    const BrowserState& browser) {
  const auto* content =
      std::get_if<std::shared_ptr<const OptionsBrowserContent>>(
          &browser.content);
  return content && *content ? content->get() : nullptr;
}

bool prepareOptionsBrowserContent(
    BrowserState& browser, const OptionsBrowserRuntimeSnapshot& runtime,
    const std::function<bool()>& cancellationRequested) {
  if (!optionsBrowserSupportsLocation(browser.location)) {
    return false;
  }
  auto prepared = std::make_shared<OptionsBrowserContent>();
  if (const OptionsBrowserContent* current = optionsBrowserContent(browser)) {
    *prepared = *current;
  }
  switch (browserOptionsPageKind(browser.location)) {
    case BrowserOptionsPageKind::Instruments:
      buildInstrumentEntries(browser.entries, browser.location, *prepared,
                             runtime, cancellationRequested);
      break;
    case BrowserOptionsPageKind::VgmMetadata:
      buildVgmMetadataEntries(browser.entries, browser.location, *prepared,
                              cancellationRequested);
      break;
    case BrowserOptionsPageKind::VgmDevices:
      buildVgmDeviceEntries(browser.entries, browser.location, *prepared,
                            runtime, cancellationRequested);
      break;
    case BrowserOptionsPageKind::VgmDevice:
      buildVgmDeviceOptionEntries(browser.entries, browser.location,
                                  *prepared, runtime, cancellationRequested);
      break;
    case BrowserOptionsPageKind::Root:
      buildOptionsEntries(browser.entries, browser.location, runtime);
      break;
  }
  browser.content = std::shared_ptr<const OptionsBrowserContent>(
      std::move(prepared));
  return true;
}

OptionsBrowserResult optionsBrowserActivateEntry(const BrowserState& browser,
                                                 const BrowserEntry& entry,
                                                 AudioPlaybackRuntime&
                                                     audioPlayback) {
  if (!optionsBrowserIsActive(browser)) {
    return OptionsBrowserResult::NotHandled;
  }
  if (entry.actionAs<browser_entry::StopInstrumentAudition>()) {
    return audioPlayback.stopKssInstrumentAudition()
               ? OptionsBrowserResult::Changed
               : OptionsBrowserResult::Handled;
  }
  if (const auto* audition =
          entry.actionAs<browser_entry::StartInstrumentAudition>()) {
    const OptionsBrowserContent* content = optionsBrowserContent(browser);
    if (content && audition->profileIndex < content->instruments.size()) {
      if (audioPlayback.startKssInstrumentAudition(
              content->instruments[audition->profileIndex])) {
        return OptionsBrowserResult::Changed;
      }
    }
    return OptionsBrowserResult::Handled;
  }
  if (const auto* option =
          entry.actionAs<browser_entry::AdjustKssOption>()) {
    return audioPlayback.adjustKssOption(option->option)
               ? OptionsBrowserResult::Changed
               : OptionsBrowserResult::Handled;
  }
  if (const auto* option =
          entry.actionAs<browser_entry::AdjustNsfOption>()) {
    return audioPlayback.adjustNsfOption(option->option)
               ? OptionsBrowserResult::Changed
               : OptionsBrowserResult::Handled;
  }
  if (const auto* option =
          entry.actionAs<browser_entry::AdjustVgmOption>()) {
    return audioPlayback.adjustVgmOption(option->option)
               ? OptionsBrowserResult::Changed
               : OptionsBrowserResult::Handled;
  }
  if (const auto* option =
          entry.actionAs<browser_entry::AdjustVgmDeviceOption>()) {
    const OptionsBrowserContent* content = optionsBrowserContent(browser);
    if (!content) {
      return OptionsBrowserResult::Handled;
    }
    const std::optional<uint32_t> deviceId =
        browserOptionsDeviceId(browser.location);
    if (!deviceId) {
      return OptionsBrowserResult::Handled;
    }
    const auto defaultOptions = content->vgmDeviceDefaults.find(*deviceId);
    const auto device = std::find_if(
        content->vgmDevices.begin(), content->vgmDevices.end(),
        [deviceId](const VgmDeviceInfo& info) {
          return info.id == *deviceId;
        });
    if (defaultOptions == content->vgmDeviceDefaults.end() ||
        device == content->vgmDevices.end()) {
      return OptionsBrowserResult::Handled;
    }
    VgmDeviceOptions baseline = defaultOptions->second;
    if (!browser.entries.empty()) {
      VgmDeviceOptions current;
      if (audioPlayback.vgmDeviceOptions(*deviceId, &current)) {
        baseline = current;
      }
    }
    return audioPlayback.adjustVgmDeviceOption(*device, baseline,
                                               option->option)
               ? OptionsBrowserResult::Changed
               : OptionsBrowserResult::Handled;
  }
  return OptionsBrowserResult::Handled;
}

std::string optionsBrowserSelectionMeta(const BrowserState& browser) {
  if (browser.entries.empty()) return "";
  int idx = std::clamp(browser.selected, 0,
                       static_cast<int>(browser.entries.size()) - 1);
  const auto& entry = browser.entries[static_cast<size_t>(idx)];
  std::string name = entry.name;
  if (entry.isDirectory() && !entry.actionAs<browser_entry::NavigateUp>()) {
    name += "/";
  }

  std::string sortLabel = "Name";
  if (browser.sortMode == BrowserState::SortMode::Date) sortLabel = "Date";
  else if (browser.sortMode == BrowserState::SortMode::Size) sortLabel = "Size";

  std::string dirArrow = browser.sortDescending ? " \xE2\x86\x93" : " \xE2\x86\x91";
  std::string metaLine = " [" + sortLabel + dirArrow + "]";

  metaLine += " Selected: " + name;
  return metaLine;
}

std::string optionsBrowserShowingLabel(const BrowserState& browser) {
  std::string label = "  Showing: ";
  if (browserOptionsPageKind(browser.location) ==
      BrowserOptionsPageKind::Instruments) {
    label += "instruments";
  } else if (browserOptionsPageKind(browser.location) ==
             BrowserOptionsPageKind::VgmDevices) {
    label += "devices";
  } else if (browserOptionsPageKind(browser.location) ==
             BrowserOptionsPageKind::VgmDevice) {
    label += "device options";
  } else if (browserOptionsPageKind(browser.location) ==
             BrowserOptionsPageKind::VgmMetadata) {
    label += "metadata";
  } else {
    label += "options";
  }
  if (!browser.location.path().empty()) {
    label += " for " + toUtf8String(browser.location.path().filename());
  }
  return label;
}
