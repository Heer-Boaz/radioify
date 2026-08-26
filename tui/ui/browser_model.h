#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "browser_location.h"
#include "consolescreen.h"

enum class KssOptionId : std::uint8_t;
enum class NsfOptionId : std::uint8_t;
enum class VgmDeviceOptionId : std::uint8_t;
enum class VgmOptionId : std::uint8_t;

struct OptionsBrowserContent;
struct TrackBrowserContent;

using BrowserContent =
    std::variant<std::monostate,
                 std::shared_ptr<const TrackBrowserContent>,
                 std::shared_ptr<const OptionsBrowserContent>>;

namespace browser_entry {

struct Information {};
struct Status {};
struct SectionHeader {};
struct NavigateUp {};
struct OpenDirectory {};
struct OpenLocation {
  BrowserLocation target;
};
struct OpenFile {};
struct PlayTrack {
  int trackIndex = 0;
};
struct AdjustKssOption {
  KssOptionId option;
};
struct AdjustNsfOption {
  NsfOptionId option;
};
struct AdjustVgmOption {
  VgmOptionId option;
};
struct AdjustVgmDeviceOption {
  VgmDeviceOptionId option;
};
struct StartInstrumentAudition {
  std::size_t profileIndex = 0;
};
struct StopInstrumentAudition {};

using Action =
    std::variant<Information, Status, SectionHeader, NavigateUp,
                 OpenDirectory, OpenLocation, OpenFile, PlayTrack,
                 AdjustKssOption, AdjustNsfOption, AdjustVgmOption,
                 AdjustVgmDeviceOption, StartInstrumentAudition,
                 StopInstrumentAudition>;

}  // namespace browser_entry

struct BrowserEntry {
  struct SortMetadata {
    std::optional<std::filesystem::file_time_type> modifiedAt;
    std::optional<std::uintmax_t> size;
  };

  BrowserEntry() = delete;
  BrowserEntry(std::string entryName, std::filesystem::path entryPath,
               browser_entry::Action entryAction)
      : name(std::move(entryName)),
        path(std::move(entryPath)),
        pathIdentity(makePathIdentity(path)),
        action(std::move(entryAction)) {}

  template <typename T>
  const T* actionAs() const {
    return std::get_if<T>(&action);
  }

  bool isDirectory() const {
    return actionAs<browser_entry::NavigateUp>() ||
           actionAs<browser_entry::OpenDirectory>() ||
           actionAs<browser_entry::OpenLocation>();
  }

  bool isMedia() const {
    return actionAs<browser_entry::OpenFile>() ||
           actionAs<browser_entry::PlayTrack>();
  }

  bool isSectionHeader() const {
    return actionAs<browser_entry::SectionHeader>() != nullptr;
  }

  bool isStatus() const {
    return actionAs<browser_entry::Status>() != nullptr;
  }

  bool isSelectable() const {
    return !isSectionHeader() && !isStatus();
  }

  bool isActivatable() const {
    return isSelectable() &&
           !actionAs<browser_entry::Information>();
  }

  std::string name;
  std::filesystem::path path;
  PathIdentity pathIdentity;
  browser_entry::Action action;
  SortMetadata sortMetadata;
};

struct BrowserState {
  struct EntryIdentity {
    std::filesystem::path path;
    PathIdentity pathIdentity;
    std::string name;
    browser_entry::Action action;
  };
  struct Location {
    BrowserLocation route;
    std::optional<EntryIdentity> selectedEntry;
    int scrollRow = 0;
  };
  struct NavigationHistoryEntry {
    Location from;
    Location to;
  };
  struct NavigationContext {
    BrowserLocationKind kind = BrowserLocationKind::Directory;
    Location origin;
    std::vector<NavigationHistoryEntry> backHistory;
    std::vector<NavigationHistoryEntry> forwardHistory;
  };
  enum class ViewportRestoreMode {
    None,
    RevealSelection,
    RestoreScroll,
  };

  BrowserLocation location;
  std::vector<BrowserEntry> entries;
  BrowserContent content;
  int selected = 0;
  int hovered = -1;
  int scrollRow = 0;
  enum class ViewMode {
    Thumbnails,
    ListPreview,
    ListOnly,
  };
  enum class SortMode {
    Name,
    Date,
    Size,
  };
  ViewMode viewMode = ViewMode::ListOnly;
  SortMode sortMode = SortMode::Name;
  bool sortDescending = false;
  std::string filter;
  bool filterActive = false;
  std::string filterBackup;
  std::string pathSearch;
  bool pathSearchActive = false;
  std::optional<NavigationContext> navigationContext;
  std::vector<NavigationHistoryEntry> backHistory;
  std::vector<NavigationHistoryEntry> forwardHistory;
  ViewportRestoreMode viewportRestoreMode = ViewportRestoreMode::None;
  int viewportRestoreScrollRow = 0;
  bool contentLoading = false;
  std::string contentError;
};

struct GridLayout {
  int rowsVisible = 0;
  int totalRows = 0;
  int cols = 0;
  int colWidth = 0;
  int cellHeight = 1;
  int thumbWidth = 0;
  int thumbHeight = 0;
  bool showThumbs = false;
  int listWidth = 0;
  bool showPreview = false;
  int previewX = 0;
  int previewWidth = 0;
  int previewHeight = 0;
  bool showScrollBar = false;
  int scrollBarX = -1;
  int scrollBarWidth = 0;
  std::vector<std::string> names;
};

void sortBrowserEntries(BrowserState& state);

GridLayout buildLayout(const BrowserState& state, int width, int listHeight);
void drawBrowserEntries(ConsoleScreen& screen,
                        const BrowserState& browser,
                        const GridLayout& layout,
                        int listTop,
                        int listHeight,
                        const Style& baseStyle,
                        const Style& normalStyle,
                        const Style& dirStyle,
                        const Style& highlightStyle,
                        const Style& hoverStyle,
                        const Style& dimStyle,
                        const Style& playbackStyle,
                        int playingEntryIndex,
                        bool (*isImage)(const std::filesystem::path&),
                        bool (*isVideo)(const std::filesystem::path&),
                        bool (*isAudio)(const std::filesystem::path&));
