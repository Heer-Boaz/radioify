#include "browser_directory_listing.h"

#include <system_error>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "media_artwork_sidecar.h"
#include "media_formats.h"
#include "runtime_helpers.h"

namespace browser_directory_listing {
namespace {

struct DriveEntry {
  std::string label;
  std::filesystem::path path;
};

bool cancellationWasRequested(
    const CancellationRequested& cancellationRequested) {
  return cancellationRequested && cancellationRequested();
}

bool shouldHideMediaMetadataFile(
    const std::filesystem::directory_entry& entry) {
#ifdef _WIN32
  const std::filesystem::path& path = entry.path();
  if (!isKnownMediaArtworkSidecarPath(path)) {
    return false;
  }

  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    return false;
  }

  return (attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
#else
  (void)entry;
  return false;
#endif
}

std::vector<DriveEntry> listDrives() {
  std::vector<DriveEntry> drives;
#ifdef _WIN32
  const DWORD mask = GetLogicalDrives();
  if (mask == 0) {
    return drives;
  }
  for (int index = 0; index < 26; ++index) {
    if ((mask & (1u << index)) == 0) {
      continue;
    }
    const char letter = static_cast<char>('A' + index);
    std::string label{letter, ':'};
    std::string root{letter, ':', '\\'};
    drives.push_back(DriveEntry{std::move(label),
                                std::filesystem::path(std::move(root))});
  }
#endif
  return drives;
}

}  // namespace

Result list(const std::filesystem::path& directory,
            CancellationRequested cancellationRequested) {
  std::vector<BrowserEntry> entries;
  std::vector<BrowserEntry> items;
  std::vector<BrowserEntry> knownFolders;
#ifdef _WIN32
  std::filesystem::path browseDirectory = directory;
  if (browseDirectory.has_root_name() &&
      !browseDirectory.has_root_directory() &&
      browseDirectory.relative_path().empty()) {
    std::string root = toUtf8String(browseDirectory.root_name());
    root.push_back('\\');
    browseDirectory = std::filesystem::path(root);
  }
#else
  const std::filesystem::path& browseDirectory = directory;
#endif

  const auto appendKnownFolder = [&](const std::string& name,
                                     const std::filesystem::path& path) {
    if (cancellationWasRequested(cancellationRequested) || path.empty()) {
      return;
    }
    std::error_code error;
    if (!std::filesystem::is_directory(path, error) || error) {
      return;
    }
    knownFolders.emplace_back(name, path, browser_entry::OpenDirectory{});
  };

  const auto appendSectionHeader = [&](const std::string& name) {
    entries.emplace_back(name, std::filesystem::path{},
                         browser_entry::SectionHeader{});
  };

  const auto appendWindowsKnownFolders = [&]() {
    std::string userProfile;
    if (const auto configuredProfile = getEnvString("USERPROFILE")) {
      userProfile = *configuredProfile;
    }
    if (userProfile.empty()) {
      const auto homeDrive = getEnvString("HOMEDRIVE");
      const auto homePath = getEnvString("HOMEPATH");
      if (!homeDrive || homeDrive->empty() || !homePath || homePath->empty()) {
        return;
      }
      userProfile = *homeDrive + *homePath;
    }

    const std::filesystem::path homePath(userProfile);
    appendKnownFolder("Home", homePath);
    appendKnownFolder("Desktop", homePath / "Desktop");
    appendKnownFolder("Documents", homePath / "Documents");
    appendKnownFolder("Downloads", homePath / "Downloads");
    appendKnownFolder("Music", homePath / "Music");
    appendKnownFolder("Pictures", homePath / "Pictures");
    appendKnownFolder("Videos", homePath / "Videos");
  };

#ifdef _WIN32
  if (browseDirectory.empty()) {
    const std::vector<DriveEntry> drives = listDrives();
    if (!drives.empty()) {
      appendSectionHeader("Drives");
      for (const DriveEntry& drive : drives) {
        entries.emplace_back(drive.label, drive.path,
                             browser_entry::OpenDirectory{});
      }
    }
    appendWindowsKnownFolders();
    if (!knownFolders.empty()) {
      appendSectionHeader("Locations");
      entries.insert(entries.end(), knownFolders.begin(), knownFolders.end());
    }
    if (cancellationWasRequested(cancellationRequested)) {
      return Cancelled{};
    }
    return entries;
  }
  if (browseDirectory == browseDirectory.root_path()) {
    entries.emplace_back("..", std::filesystem::path{},
                         browser_entry::NavigateUp{});
  }
#endif

  if (browseDirectory.has_parent_path() &&
      browseDirectory != browseDirectory.root_path()) {
    entries.emplace_back("..", browseDirectory.parent_path(),
                         browser_entry::NavigateUp{});
  }

  std::error_code iteratorError;
  std::filesystem::directory_iterator iterator(
      browseDirectory, std::filesystem::directory_options::none,
      iteratorError);
  if (iteratorError) {
    return Error{"Unable to open this folder: " + iteratorError.message()};
  }

  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    if (cancellationWasRequested(cancellationRequested)) {
      return Cancelled{};
    }

    const std::filesystem::directory_entry& entry = *iterator;
    const std::filesystem::path& path = entry.path();
    std::error_code metadataError;
    if (entry.is_directory(metadataError) && !metadataError) {
      BrowserEntry item{toUtf8String(path.filename()), path,
                        browser_entry::OpenDirectory{}};
      item.sortMetadata.modifiedAt = entry.last_write_time(metadataError);
      if (metadataError) {
        item.sortMetadata.modifiedAt.reset();
      }
      items.push_back(std::move(item));
    } else if (entry.is_regular_file(metadataError) && !metadataError &&
               isSupportedMediaExt(path) &&
               !shouldHideMediaMetadataFile(entry)) {
      BrowserEntry item{toUtf8String(path.filename()), path,
                        browser_entry::OpenFile{}};
      item.sortMetadata.modifiedAt = entry.last_write_time(metadataError);
      if (metadataError) {
        item.sortMetadata.modifiedAt.reset();
      }
      item.sortMetadata.size = entry.file_size(metadataError);
      if (metadataError) {
        item.sortMetadata.size.reset();
      }
      items.push_back(std::move(item));
    }

    iterator.increment(iteratorError);
    if (iteratorError) {
      return Error{"Unable to enumerate this folder: " +
                   iteratorError.message()};
    }
  }

  if (cancellationWasRequested(cancellationRequested)) {
    return Cancelled{};
  }

  entries.insert(entries.end(), items.begin(), items.end());
  return entries;
}

}  // namespace browser_directory_listing
