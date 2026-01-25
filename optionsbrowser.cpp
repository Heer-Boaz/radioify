#include "optionsbrowser.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "audioplayback.h"
#include "consoleinput.h"
#include "kssoptions.h"
#include "nsfoptions.h"
#include "ui_helpers.h"

namespace {
enum class OptionsTarget {
  None,
  Kss,
  Nsf,
};

enum class OptionsBrowserMode {
  Root,
  Instruments,
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
};

OptionsBrowserState gOptionsBrowser;

std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

bool isKssExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".kss";
}

bool isNsfExt(const std::filesystem::path& p) {
  std::string ext = toLower(p.extension().string());
  return ext == ".nsf";
}

OptionsTarget targetForPath(const std::filesystem::path& p) {
  if (isKssExt(p)) return OptionsTarget::Kss;
  if (isNsfExt(p)) return OptionsTarget::Nsf;
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

std::string hex32(uint32_t value) {
  char buf[11];
  std::snprintf(buf, sizeof(buf), "0x%08x", value);
  return std::string(buf);
}

std::filesystem::path instrumentsPath() {
  if (gOptionsBrowser.path.empty()) return {};
  return gOptionsBrowser.path / "Instruments";
}

bool isOptionsPath(const std::filesystem::path& path) {
  if (!gOptionsBrowser.active) return false;
  return path == gOptionsBrowser.path || path == instrumentsPath();
}

OptionsBrowserMode modeForPath(const std::filesystem::path& path) {
  if (path == instrumentsPath()) return OptionsBrowserMode::Instruments;
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
  if (browser.filterActive || !browser.filter.empty()) {
    metaLine += " [Filter: " + browser.filter +
                (browser.filterActive ? "_" : "") + "]";
  }

  metaLine += " Selected: " + name;
  return metaLine;
}

std::string optionsBrowserShowingLabel() {
  std::string label = "  Showing: ";
  if (gOptionsBrowser.mode == OptionsBrowserMode::Instruments) {
    label += "instruments";
  } else {
    label += "options";
  }
  if (!gOptionsBrowser.file.empty()) {
    label += " for " + toUtf8String(gOptionsBrowser.file.filename());
  }
  return label;
}
