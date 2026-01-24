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
  HANDLE handle_ = INVALID_HANDLE_VALUE;
  DWORD originalMode_ = 0;
  bool active_ = false;
};

struct FileEntry {
  std::string name;
  std::filesystem::path path;
  bool isDir = false;
  int trackIndex = -1;
  int optionId = -1;
};

struct BrowserState {
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
  std::function<void()> onTogglePause;
  std::function<void()> onToggleRadio;
  std::function<void()> onToggle50Hz;
  std::function<void()> onToggleOptions;
  std::function<void()> onToggleVsync;
  std::function<void(int)> onSeekBy;
  std::function<void(double)> onSeekToRatio;
  std::function<void(float)> onAdjustVolume;
  std::function<bool(const std::filesystem::path&)> onPlayFile;
  std::function<void(const std::filesystem::path&)> onRenderFile;
  std::function<void(BrowserState&, const std::string&)> onRefreshBrowser;
};

bool handlePlaybackInput(const InputEvent& ev, bool& running,
                          const InputCallbacks& callbacks);

void handleInputEvent(
  const InputEvent& ev,
  BrowserState& browser,
  const GridLayout& layout,
  const BreadcrumbLine& breadcrumbLine,
  int breadcrumbY,
  int listTop,
  int listHeight,
  int progressBarX,
  int progressBarY,
  int progressBarWidth,
  bool playMode,
  bool decoderReady,
  int& breadcrumbHover,
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
