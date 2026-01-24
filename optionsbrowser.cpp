#include "optionsbrowser.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>

#include "audioplayback.h"
#include "consoleinput.h"
#include "kssoptions.h"
#include "ui_helpers.h"

namespace {
struct OptionsBrowserState {
  bool active = false;
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

std::filesystem::path resolveOptionsFile(const BrowserState& browser) {
  if (!browser.entries.empty()) {
    int idx = std::clamp(browser.selected, 0,
                         static_cast<int>(browser.entries.size()) - 1);
    const auto& entry = browser.entries[static_cast<size_t>(idx)];
    if (!entry.isDir && isKssExt(entry.path)) {
      return entry.path;
    }
  }
  std::filesystem::path nowPlaying = audioGetNowPlaying();
  if (isKssExt(nowPlaying)) {
    return nowPlaying;
  }
  return {};
}

void buildOptionsEntries(std::vector<FileEntry>& entries) {
  entries.clear();
  if (!gOptionsBrowser.returnDir.empty()) {
    entries.push_back(FileEntry{"..", gOptionsBrowser.returnDir, true});
  }

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
}
}  // namespace

bool optionsBrowserIsActive(const BrowserState& browser) {
  return gOptionsBrowser.active && browser.dir == gOptionsBrowser.path;
}

bool optionsBrowserRefresh(BrowserState& browser) {
  if (!optionsBrowserIsActive(browser)) {
    if (gOptionsBrowser.active && browser.dir != gOptionsBrowser.path) {
      gOptionsBrowser.active = false;
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
    if (audioAdjustKssOption(static_cast<KssOptionId>(entry.optionId))) {
      return OptionsBrowserResult::Changed;
    }
  }
  return OptionsBrowserResult::Handled;
}

void optionsBrowserToggle(BrowserState& browser) {
  if (optionsBrowserIsActive(browser)) {
    browser.dir = gOptionsBrowser.returnDir;
    gOptionsBrowser.active = false;
    return;
  }

  gOptionsBrowser.active = true;
  gOptionsBrowser.returnDir = browser.dir;
  gOptionsBrowser.file = resolveOptionsFile(browser);
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
