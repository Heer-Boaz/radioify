#pragma once

#include <cstdint>
#include <filesystem>
#include <utility>

enum class BrowserLocationKind {
  Directory,
  TrackBrowser,
  OptionsBrowser,
};

enum class BrowserOptionsPage {
  Root,
  Instruments,
  VgmDevices,
  VgmDevice,
  VgmMetadata,
};

struct BrowserLocation {
  BrowserLocationKind kind = BrowserLocationKind::Directory;
  std::filesystem::path path;
  BrowserOptionsPage optionsPage = BrowserOptionsPage::Root;
  int trackIndex = -1;
  uint32_t deviceId = 0;
};

inline BrowserLocation browserDirectoryLocation(std::filesystem::path dir) {
  BrowserLocation location;
  location.path = std::move(dir);
  return location;
}

inline BrowserLocation browserTrackLocation(std::filesystem::path file) {
  BrowserLocation location;
  location.kind = BrowserLocationKind::TrackBrowser;
  location.path = std::move(file);
  return location;
}

inline BrowserLocation browserOptionsLocation(
    std::filesystem::path file, int trackIndex = -1,
    BrowserOptionsPage page = BrowserOptionsPage::Root,
    uint32_t deviceId = 0) {
  BrowserLocation location;
  location.kind = BrowserLocationKind::OptionsBrowser;
  location.path = std::move(file);
  location.optionsPage = page;
  location.trackIndex = trackIndex;
  location.deviceId = deviceId;
  return location;
}

inline bool operator==(const BrowserLocation& left,
                       const BrowserLocation& right) {
  if (left.kind != right.kind || left.path != right.path) {
    return false;
  }
  if (left.kind != BrowserLocationKind::OptionsBrowser) {
    return true;
  }
  return left.optionsPage == right.optionsPage &&
         left.trackIndex == right.trackIndex &&
         left.deviceId == right.deviceId;
}

inline bool operator!=(const BrowserLocation& left,
                       const BrowserLocation& right) {
  return !(left == right);
}
