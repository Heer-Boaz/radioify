#include "optionsbrowser.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

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

struct OptionsBrowserState {
  bool active = false;
  OptionsTarget target = OptionsTarget::None;
  std::filesystem::path path;
  std::filesystem::path returnDir;
  std::filesystem::path file;
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
    addOption(KssOptionId::SccType,
              "SCC type: " + sccTypeLabel(options.sccType));
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
}  // namespace

bool optionsBrowserIsActive(const BrowserState& browser) {
  return gOptionsBrowser.active && browser.dir == gOptionsBrowser.path;
}

bool optionsBrowserCanToggle(const BrowserState& browser) {
  if (optionsBrowserIsActive(browser)) return true;
  OptionsTarget target = OptionsTarget::None;
  return !resolveOptionsFile(browser, &target).empty();
}

bool optionsBrowserRefresh(BrowserState& browser) {
  if (!optionsBrowserIsActive(browser)) {
    if (gOptionsBrowser.active && browser.dir != gOptionsBrowser.path) {
      gOptionsBrowser.active = false;
      gOptionsBrowser.target = OptionsTarget::None;
    }
    return false;
  }
  buildOptionsEntries(browser.entries);
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
    return;
  }

  OptionsTarget target = OptionsTarget::None;
  std::filesystem::path file = resolveOptionsFile(browser, &target);
  if (file.empty() || target == OptionsTarget::None) {
    return;
  }
  gOptionsBrowser.active = true;
  gOptionsBrowser.target = target;
  gOptionsBrowser.returnDir = browser.dir;
  gOptionsBrowser.file = file;
  gOptionsBrowser.path = browser.dir / "Options";
  browser.dir = gOptionsBrowser.path;
  browser.selected = 0;
  browser.scrollRow = 0;
  browser.filter.clear();
  browser.filterActive = false;
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
  std::string label = "  Showing: options";
  if (!gOptionsBrowser.file.empty()) {
    label += " for " + toUtf8String(gOptionsBrowser.file.filename());
  }
  return label;
}
