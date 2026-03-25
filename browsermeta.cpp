#include "browsermeta.h"

#include <algorithm>
#include <ctime>
#include <unordered_map>

#include "consoleinput.h"
#include "ui_helpers.h"
#include "videodecoder.h"

namespace {
std::string formatBytes(uintmax_t bytes) {
  const char* suffixes[] = {"B", "KB", "MB", "GB", "TB"};
  double value = static_cast<double>(bytes);
  int idx = 0;
  while (value >= 1024.0 && idx < 4) {
    value /= 1024.0;
    ++idx;
  }
  char buf[32];
  if (idx == 0) {
    std::snprintf(buf, sizeof(buf), "%llu B",
                  static_cast<unsigned long long>(bytes));
  } else if (value >= 10.0) {
    std::snprintf(buf, sizeof(buf), "%.0f %s", value, suffixes[idx]);
  } else {
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, suffixes[idx]);
  }
  return buf;
}

std::string formatFileTime(const std::filesystem::file_time_type& ft) {
  using namespace std::chrono;
  auto sctp = time_point_cast<system_clock::duration>(
      ft - std::filesystem::file_time_type::clock::now() +
      system_clock::now());
  std::time_t tt = system_clock::to_time_t(sctp);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  char buf[32];
  if (std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm) == 0) {
    return "";
  }
  return buf;
}

struct SelectionMetaCacheEntry {
  bool initialized = false;
  bool isDir = false;
  bool videoAttempted = false;
  std::string sizeLabel;
  std::string timeLabel;
  int width = 0;
  int height = 0;
  int64_t bitRate = 0;
  bool isHDR = false;
  std::string codec;
  std::string duration;
};
}  // namespace

std::string buildSelectionMeta(const BrowserState& browser,
                               bool (*isVideo)(const std::filesystem::path&)) {
  static std::unordered_map<std::string, SelectionMetaCacheEntry> cache;
  if (browser.entries.empty()) return "";
  int idx = std::clamp(browser.selected, 0,
                       static_cast<int>(browser.entries.size()) - 1);
  const auto& entry = browser.entries[static_cast<size_t>(idx)];
  std::string name = entry.name;
  if (entry.isDir && name != "..") name += "/";

  std::string key = toUtf8String(entry.path);
  auto it = cache.find(key);
  if (it == cache.end()) {
    SelectionMetaCacheEntry fresh;
    fresh.isDir = entry.isDir;
    try {
      if (entry.isDir) {
        fresh.sizeLabel = "<DIR>";
      } else {
        uintmax_t size = std::filesystem::file_size(entry.path);
        fresh.sizeLabel = formatBytes(size);
      }
    } catch (...) {
      fresh.sizeLabel = "?";
    }
    try {
      fresh.timeLabel =
          formatFileTime(std::filesystem::last_write_time(entry.path));
    } catch (...) {
      fresh.timeLabel.clear();
    }
    fresh.initialized = true;
    it = cache.emplace(key, std::move(fresh)).first;
  }

  SelectionMetaCacheEntry& meta = it->second;
  if (!meta.videoAttempted && !entry.isDir && isVideo &&
      isVideo(entry.path)) {
    VideoMetadata vmeta;
    std::string error;
    if (probeVideoMetadata(entry.path, &vmeta, &error)) {
      meta.width = vmeta.width;
      meta.height = vmeta.height;
      meta.bitRate = vmeta.bitRate;
      meta.isHDR = vmeta.isHDR;
      if (vmeta.duration100ns > 0) {
        double seconds = static_cast<double>(vmeta.duration100ns) / 10000000.0;
        meta.duration = formatTime(seconds);
      }
      meta.codec = vmeta.codecName;
    }
    meta.videoAttempted = true;
  }

  std::string sortLabel = "Name";
  if (browser.sortMode == BrowserState::SortMode::Date) sortLabel = "Date";
  else if (browser.sortMode == BrowserState::SortMode::Size) sortLabel = "Size";

  std::string dirArrow = browser.sortDescending ? " \xE2\x86\x93" : " \xE2\x86\x91"; // Down/Up arrows
  std::string metaLine = " [" + sortLabel + dirArrow + "]";

  metaLine += " Selected: " + name;
  if (!meta.sizeLabel.empty()) metaLine += "  " + meta.sizeLabel;
  if (meta.width > 0 && meta.height > 0) {
    metaLine += "  " + std::to_string(meta.width) + "x" +
                std::to_string(meta.height);
    if (meta.isHDR) metaLine += " HDR";
    if (meta.bitRate > 0) {
      metaLine += "  " + formatBytes(static_cast<uintmax_t>(meta.bitRate / 8)) + "/s";
    }
  }
  if (!meta.duration.empty()) metaLine += "  " + meta.duration;
  if (!meta.codec.empty()) metaLine += "  " + meta.codec;
  if (!meta.timeLabel.empty()) metaLine += "  " + meta.timeLabel;
  return metaLine;
}
