#include "browser_model.h"

#include <algorithm>
#include <cctype>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {

std::string normalizedSortName(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return value;
}

template <typename T>
int compareOptional(const std::optional<T>& left,
                    const std::optional<T>& right) {
  if (left.has_value() != right.has_value()) {
    return left ? -1 : 1;
  }
  if (!left) {
    return 0;
  }
  if (*left < *right) {
    return -1;
  }
  if (*right < *left) {
    return 1;
  }
  return 0;
}

int compareText(const std::string& left, const std::string& right) {
  if (left < right) {
    return -1;
  }
  if (right < left) {
    return 1;
  }
  return 0;
}

int compareSortKeys(const BrowserEntry& left, const BrowserEntry& right,
                    BrowserState::SortMode mode) {
  int result = 0;
  if (mode == BrowserState::SortMode::Date) {
    result = compareOptional(left.sortMetadata.modifiedAt,
                             right.sortMetadata.modifiedAt);
  } else if (mode == BrowserState::SortMode::Size &&
             !left.isDirectory() && !right.isDirectory()) {
    result = compareOptional(left.sortMetadata.size,
                             right.sortMetadata.size);
  }
  if (result != 0) {
    return result;
  }

  result = compareText(normalizedSortName(left.name),
                       normalizedSortName(right.name));
  if (result != 0) {
    return result;
  }
  result = compareText(left.name, right.name);
  if (result != 0) {
    return result;
  }

  const auto& leftPath = left.pathIdentity.normalizedPath.native();
  const auto& rightPath = right.pathIdentity.normalizedPath.native();
  if (leftPath < rightPath) {
    return -1;
  }
  if (rightPath < leftPath) {
    return 1;
  }
  return 0;
}

#ifdef _WIN32
bool isDriveEntry(const BrowserEntry& entry) {
  if (entry.path.empty()) {
    return false;
  }
  return entry.path.has_root_name() && entry.path.has_root_directory() &&
         entry.path.relative_path().empty();
}
#else
bool isDriveEntry(const BrowserEntry&) { return false; }
#endif

}  // namespace

std::vector<DriveEntry> listDriveEntries() {
  std::vector<DriveEntry> drives;
#ifdef _WIN32
  DWORD mask = GetLogicalDrives();
  if (mask == 0) return drives;
  for (int i = 0; i < 26; ++i) {
    if ((mask & (1u << i)) == 0) continue;
    char letter = static_cast<char>('A' + i);
    std::string label;
    label.push_back(letter);
    label.push_back(':');
    std::string root;
    root.push_back(letter);
    root.append(":\\");
    drives.push_back(DriveEntry{label, std::filesystem::path(root)});
  }
#endif
  return drives;
}

void sortBrowserEntries(BrowserState& state) {
  if (state.entries.empty()) {
    return;
  }

  const bool sortingRootLevel = state.location.path().empty();
  const bool hasSectionHeaders =
      std::any_of(state.entries.begin(), state.entries.end(),
                  [](const BrowserEntry& entry) {
                    return entry.isSectionHeader();
                  });
  auto less = [&](const BrowserEntry& left, const BrowserEntry& right) {
    if (left.isDirectory() != right.isDirectory()) {
      return left.isDirectory();
    }
    if (sortingRootLevel && !hasSectionHeaders &&
        isDriveEntry(left) != isDriveEntry(right)) {
      return isDriveEntry(left);
    }

    const int keyOrder = compareSortKeys(left, right, state.sortMode);
    return state.sortDescending ? keyOrder > 0 : keyOrder < 0;
  };
  auto sortSection = [&](std::size_t begin, std::size_t end) {
    if (end <= begin + 1) {
      return;
    }
    std::stable_sort(state.entries.begin() + static_cast<std::ptrdiff_t>(begin),
                     state.entries.begin() + static_cast<std::ptrdiff_t>(end),
                     less);
  };

  std::size_t sectionStart = 0;
  if (state.entries.front().actionAs<browser_entry::NavigateUp>()) {
    ++sectionStart;
  }
  while (sectionStart < state.entries.size()) {
    if (state.entries[sectionStart].isSectionHeader()) {
      ++sectionStart;
      continue;
    }
    std::size_t sectionEnd = sectionStart;
    while (sectionEnd < state.entries.size() &&
           !state.entries[sectionEnd].isSectionHeader()) {
      ++sectionEnd;
    }
    sortSection(sectionStart, sectionEnd);
    sectionStart = sectionEnd;
  }
}
