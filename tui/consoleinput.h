#ifndef CONSOLEINPUT_H
#define CONSOLEINPUT_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "consolescreen.h"

struct BreadcrumbLine;

struct KeyEvent {
  WORD vk = 0;
  char ch = 0;
  DWORD control = 0;
};

struct MouseEvent {
  COORD pos{};
  DWORD buttonState = 0;
  DWORD eventFlags = 0;
  DWORD control = 0;
};

struct InputEvent {
  enum class Type {
    None,
    Key,
    Mouse,
    Resize,
  };

  Type type = Type::None;
  KeyEvent key{};
  MouseEvent mouse{};
  COORD size{};
};

class ConsoleInput {
 public:
  void init();
  void restore();
  bool poll(InputEvent& out);
  bool active() const;

 private:
  bool hasInputFocus() const;
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  DWORD originalMode_ = 0;
  bool active_ = false;
  bool xButton1Prev_ = false;
  bool xButton2Prev_ = false;
  bool focusActive_ = true;
};

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

struct InputCallbacks {
  std::function<void()> onQuit;
  std::function<void()> onResize;
  std::function<void()> onStopPlayback;
  std::function<std::filesystem::path()> onCurrentPlaybackFile;
  std::function<void()> onTogglePause;
  std::function<void()> onToggleWindow;
  std::function<void()> onToggleRadio;
  std::function<void()> onToggle50Hz;
  std::function<void()> onToggleSubtitles;
  std::function<void()> onToggleAudioTrack;
  std::function<void()> onToggleOptions;
  std::function<void()> onToggleMelodyVisualization;
  std::function<void(int)> onSeekBy;
  std::function<void(double)> onSeekToRatio;
  std::function<void(float)> onAdjustVolume;
  std::function<bool(const std::filesystem::path&)> onPlayFile;
  std::function<void(const FileEntry&, int, int)> onOpenFileContextMenu;
  std::function<void(const std::filesystem::path&)> onRenderFile;
  std::function<void(BrowserState&, const std::string&)> onRefreshBrowser;
};

enum class BrowserSearchFocus {
  None,
  Filter,
  PathSearch,
};

void setBrowserSearchFocus(BrowserState& browser, BrowserSearchFocus focus,
                          bool& dirty);

enum class ActionStripItem {
  Radio,
  Hz50,
  View,
  MelodyViz,
  Options
};

struct ActionStripButton {
  ActionStripItem id = ActionStripItem::Radio;
  int x0 = 0;
  int x1 = 0;
};

struct ActionStripLayout {
  int y = -1;
  std::vector<ActionStripButton> buttons;
};

bool handlePlaybackInput(const InputEvent& ev,
                         const InputCallbacks& callbacks);

void handleInputEvent(
  const InputEvent& ev,
  BrowserState& browser,
  const GridLayout& layout,
  const BreadcrumbLine& breadcrumbLine,
  int breadcrumbY,
  int searchBarY,
  int searchBarWidth,
  int listTop,
  int listHeight,
  int progressBarX,
  int progressBarY,
  int progressBarWidth,
  const ActionStripLayout& actionStrip,
  bool browserInteractionEnabled,
  bool playMode,
  bool decoderReady,
  int& breadcrumbHover,
  int& actionHover,
  bool& searchBarHover,
  bool& dirty,
  bool& running,
  const InputCallbacks& callbacks
);

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

#endif
