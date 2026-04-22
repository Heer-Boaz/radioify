#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "consolescreen.h"

struct FileEntry {
  std::string name;
  std::filesystem::path path;
  bool isDir = false;
  bool isSectionHeader = false;
  int trackIndex = -1;
  int optionId = -1;
  int instrumentDevice = -1;
  int instrumentChannel = -1;
  int auditionDevice = -1;
  int auditionChannel = -1;
  int auditionIndex = -1;
};

struct BrowserState {
  enum class HistoryActionType {
    EnterDirectory,
    PlayFile,
  };
  struct HistoryAction {
    HistoryActionType type = HistoryActionType::EnterDirectory;
    std::filesystem::path fromPath;
    std::filesystem::path toPath;
  };

  std::filesystem::path dir;
  std::vector<FileEntry> entries;
  int selected = 0;
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
  std::vector<HistoryAction> backHistory;
  std::vector<HistoryAction> forwardHistory;
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

struct DriveEntry {
  std::string label;
  std::filesystem::path path;
};

std::vector<DriveEntry> listDriveEntries();

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
                        const Style& dimStyle,
                        bool (*isImage)(const std::filesystem::path&),
                        bool (*isVideo)(const std::filesystem::path&),
                        bool (*isAudio)(const std::filesystem::path&));
