#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>

#include "core/path_identity.h"

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
  PathIdentity pathIdentity;
  BrowserOptionsPage optionsPage = BrowserOptionsPage::Root;
  int trackIndex = -1;
  uint32_t deviceId = 0;
};

inline BrowserLocation browserDirectoryLocation(std::filesystem::path dir) {
  BrowserLocation location;
  location.path = std::move(dir);
  location.pathIdentity = makePathIdentity(location.path);
  return location;
}

inline BrowserLocation browserTrackLocation(std::filesystem::path file) {
  BrowserLocation location;
  location.kind = BrowserLocationKind::TrackBrowser;
  location.path = std::move(file);
  location.pathIdentity = makePathIdentity(location.path);
  return location;
}

inline BrowserLocation browserOptionsLocation(
    std::filesystem::path file, int trackIndex = -1,
    BrowserOptionsPage page = BrowserOptionsPage::Root,
    uint32_t deviceId = 0) {
  BrowserLocation location;
  location.kind = BrowserLocationKind::OptionsBrowser;
  location.path = std::move(file);
  location.pathIdentity = makePathIdentity(location.path);
  location.optionsPage = page;
  location.trackIndex = trackIndex;
  location.deviceId = deviceId;
  return location;
}

inline bool browserLocationIsContextual(const BrowserLocation& location) {
  return location.kind == BrowserLocationKind::OptionsBrowser;
}

inline std::optional<BrowserLocation> browserOptionsParentLocation(
    const BrowserLocation& location) {
  if (location.kind != BrowserLocationKind::OptionsBrowser) {
    return std::nullopt;
  }

  BrowserOptionsPage parent = BrowserOptionsPage::Root;
  switch (location.optionsPage) {
    case BrowserOptionsPage::Root:
      return std::nullopt;
    case BrowserOptionsPage::Instruments:
    case BrowserOptionsPage::VgmDevices:
    case BrowserOptionsPage::VgmMetadata:
      parent = BrowserOptionsPage::Root;
      break;
    case BrowserOptionsPage::VgmDevice:
      parent = BrowserOptionsPage::VgmDevices;
      break;
  }
  return browserOptionsLocation(location.path, location.trackIndex, parent);
}

inline bool operator==(const BrowserLocation& left,
                       const BrowserLocation& right) {
  if (left.kind != right.kind || left.pathIdentity != right.pathIdentity) {
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
