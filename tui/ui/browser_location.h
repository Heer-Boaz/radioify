#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <utility>
#include <variant>

#include "core/path_identity.h"

enum class BrowserLocationKind {
  Directory,
  TrackBrowser,
  OptionsBrowser,
};

enum class BrowserOptionsPageKind {
  Root,
  Instruments,
  VgmDevices,
  VgmDevice,
  VgmMetadata,
};

struct BrowserOptionsRoot {};
struct BrowserOptionsInstruments {
  uint32_t trackIndex;
};
struct BrowserOptionsVgmDevices {};
struct BrowserOptionsVgmDevice {
  uint32_t deviceId;
};
struct BrowserOptionsVgmMetadata {};

using BrowserOptionsPage =
    std::variant<BrowserOptionsRoot, BrowserOptionsInstruments,
                 BrowserOptionsVgmDevices, BrowserOptionsVgmDevice,
                 BrowserOptionsVgmMetadata>;

struct DirectoryBrowserLocation {
  std::filesystem::path path;
  PathIdentity pathIdentity;
};

struct TrackBrowserLocation {
  std::filesystem::path path;
  PathIdentity pathIdentity;
};

struct OptionsBrowserLocation {
  std::filesystem::path path;
  PathIdentity pathIdentity;
  std::optional<uint32_t> trackIndex;
  BrowserOptionsPage page;
};

class BrowserLocation {
 public:
  BrowserLocation() = default;

  BrowserLocationKind kind() const {
    if (std::holds_alternative<DirectoryBrowserLocation>(value_)) {
      return BrowserLocationKind::Directory;
    }
    if (std::holds_alternative<TrackBrowserLocation>(value_)) {
      return BrowserLocationKind::TrackBrowser;
    }
    return BrowserLocationKind::OptionsBrowser;
  }

  const std::filesystem::path& path() const {
    return std::visit(
        [](const auto& location) -> const std::filesystem::path& {
          return location.path;
        },
        value_);
  }

  const PathIdentity& pathIdentity() const {
    return std::visit(
        [](const auto& location) -> const PathIdentity& {
          return location.pathIdentity;
        },
        value_);
  }

  const OptionsBrowserLocation* options() const {
    return std::get_if<OptionsBrowserLocation>(&value_);
  }

 private:
  using Value = std::variant<DirectoryBrowserLocation, TrackBrowserLocation,
                             OptionsBrowserLocation>;

  explicit BrowserLocation(Value value) : value_(std::move(value)) {}

  Value value_ = DirectoryBrowserLocation{};

  friend BrowserLocation browserDirectoryLocation(std::filesystem::path dir);
  friend BrowserLocation browserTrackLocation(std::filesystem::path file);
  friend BrowserLocation browserOptionsLocation(
      std::filesystem::path file, std::optional<uint32_t> trackIndex,
      BrowserOptionsPage page);
};

inline BrowserLocation browserDirectoryLocation(std::filesystem::path dir) {
  PathIdentity identity = makePathIdentity(dir);
  return BrowserLocation(
      DirectoryBrowserLocation{std::move(dir), std::move(identity)});
}

inline BrowserLocation browserTrackLocation(std::filesystem::path file) {
  PathIdentity identity = makePathIdentity(file);
  return BrowserLocation(
      TrackBrowserLocation{std::move(file), std::move(identity)});
}

inline BrowserLocation browserOptionsLocation(
    std::filesystem::path file,
    std::optional<uint32_t> trackIndex = std::nullopt,
    BrowserOptionsPage page = BrowserOptionsRoot{}) {
  PathIdentity identity = makePathIdentity(file);
  return BrowserLocation(OptionsBrowserLocation{
      std::move(file), std::move(identity), trackIndex, std::move(page)});
}

inline BrowserOptionsPageKind browserOptionsPageKind(
    const BrowserOptionsPage& page) {
  if (std::holds_alternative<BrowserOptionsInstruments>(page)) {
    return BrowserOptionsPageKind::Instruments;
  }
  if (std::holds_alternative<BrowserOptionsVgmDevices>(page)) {
    return BrowserOptionsPageKind::VgmDevices;
  }
  if (std::holds_alternative<BrowserOptionsVgmDevice>(page)) {
    return BrowserOptionsPageKind::VgmDevice;
  }
  if (std::holds_alternative<BrowserOptionsVgmMetadata>(page)) {
    return BrowserOptionsPageKind::VgmMetadata;
  }
  return BrowserOptionsPageKind::Root;
}

inline BrowserOptionsPageKind browserOptionsPageKind(
    const BrowserLocation& location) {
  const OptionsBrowserLocation* options = location.options();
  return options ? browserOptionsPageKind(options->page)
                 : BrowserOptionsPageKind::Root;
}

inline std::optional<uint32_t> browserOptionsTrackIndex(
    const BrowserLocation& location) {
  const OptionsBrowserLocation* options = location.options();
  return options ? options->trackIndex : std::nullopt;
}

inline std::optional<uint32_t> browserOptionsDeviceId(
    const BrowserLocation& location) {
  const OptionsBrowserLocation* options = location.options();
  if (!options) {
    return std::nullopt;
  }
  const auto* device = std::get_if<BrowserOptionsVgmDevice>(&options->page);
  return device ? std::optional<uint32_t>(device->deviceId) : std::nullopt;
}

inline std::optional<uint32_t> browserOptionsInstrumentTrackIndex(
    const BrowserLocation& location) {
  const OptionsBrowserLocation* options = location.options();
  if (!options) {
    return std::nullopt;
  }
  const auto* instruments =
      std::get_if<BrowserOptionsInstruments>(&options->page);
  return instruments ? std::optional<uint32_t>(instruments->trackIndex)
                     : std::nullopt;
}

inline bool browserLocationIsContextual(const BrowserLocation& location) {
  return location.kind() == BrowserLocationKind::OptionsBrowser;
}

inline std::optional<BrowserLocation> browserOptionsParentLocation(
    const BrowserLocation& location) {
  const OptionsBrowserLocation* options = location.options();
  if (!options) {
    return std::nullopt;
  }

  BrowserOptionsPage parent = BrowserOptionsRoot{};
  switch (browserOptionsPageKind(options->page)) {
    case BrowserOptionsPageKind::Root:
      return std::nullopt;
    case BrowserOptionsPageKind::Instruments:
    case BrowserOptionsPageKind::VgmDevices:
    case BrowserOptionsPageKind::VgmMetadata:
      parent = BrowserOptionsRoot{};
      break;
    case BrowserOptionsPageKind::VgmDevice:
      parent = BrowserOptionsVgmDevices{};
      break;
  }
  return browserOptionsLocation(options->path, options->trackIndex,
                                std::move(parent));
}

inline bool operator==(const BrowserLocation& left,
                       const BrowserLocation& right) {
  if (left.kind() != right.kind() ||
      left.pathIdentity() != right.pathIdentity()) {
    return false;
  }
  const OptionsBrowserLocation* leftOptions = left.options();
  const OptionsBrowserLocation* rightOptions = right.options();
  if (!leftOptions || !rightOptions) {
    return true;
  }
  if (leftOptions->trackIndex != rightOptions->trackIndex ||
      leftOptions->page.index() != rightOptions->page.index()) {
    return false;
  }
  return browserOptionsDeviceId(left) == browserOptionsDeviceId(right) &&
         browserOptionsInstrumentTrackIndex(left) ==
             browserOptionsInstrumentTrackIndex(right);
}

inline bool operator!=(const BrowserLocation& left,
                       const BrowserLocation& right) {
  return !(left == right);
}
