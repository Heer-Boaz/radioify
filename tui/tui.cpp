#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app_common.h"
#include "audio_mini_player.h"
#include "asciiart.h"
#include "audioplayback.h"
#include "browser_model.h"
#include "browsermeta.h"
#include "consoleinput.h"
#include "consolescreen.h"
#include "input_file_drop.h"
#include "media_artwork_sidecar.h"
#include "core/windows_message_pump.h"
#include "m4adecoder.h"
#include "miniaudio.h"
#include "optionsbrowser.h"
#include "playback_dialog.h"
#include "calibration_report.h"
#include "radio.h"
#include "audiofilter/radio1938/radio_buffer_io.h"
#include "audiofilter/radio1938/preview/radio_preview_pipeline.h"
#include "playback/playback_control_command.h"
#include "playback/playback_shortcuts.h"
#include "playback/overlay/playback_overlay.h"
#include "playback/system_media_transport_controls.h"
#include "playback_target_resolver.h"
#include "playback_transport_navigation.h"
#include "tracklist.h"
#include "track_browser_state.h"
#include "loopsplit_cli.h"
#include "loopsplit_ui.h"
#include "tui_export.h"
#include "ui_footer_layout.h"
#include "ui_helpers.h"
#include "ui_inputlogic.h"
#include "ui_input_pump.h"
#include "ui_viewport.h"
#include "videoplayback.h"
#include "videowindow.h"
#include "media_formats.h"
#include "runtime_helpers.h"

#include "tui.h"
#include "timing_log.h"

static std::string toLower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return s;
}

enum class UiDirtyFlags : uint32_t {
  None = 0,
  Frame = 1u << 0,
  Layout = 1u << 1,
  Async = 1u << 2,
};

inline UiDirtyFlags operator|(UiDirtyFlags a, UiDirtyFlags b) {
  return static_cast<UiDirtyFlags>(static_cast<uint32_t>(a) |
                                   static_cast<uint32_t>(b));
}

inline UiDirtyFlags& operator|=(UiDirtyFlags& a, UiDirtyFlags b) {
  a = a | b;
  return a;
}

inline bool hasDirtyFlag(UiDirtyFlags value, UiDirtyFlags flag) {
  return (static_cast<uint32_t>(value) & static_cast<uint32_t>(flag)) != 0;
}

static DWORD waitForBrowserWake(ConsoleInput& input, HANDLE asyncWakeHandle,
                                DWORD timeoutMs) {
  std::vector<HANDLE> handles;
  if (HANDLE inputHandle = input.waitHandle()) {
    handles.push_back(inputHandle);
  }
  if (asyncWakeHandle) {
    handles.push_back(asyncWakeHandle);
  }
  return waitForHandlesAndPumpThreadWindowMessages(
      static_cast<DWORD>(handles.size()),
      handles.empty() ? nullptr : handles.data(), timeoutMs);
}

static bool isVideoExt(const std::filesystem::path& p) {
  return isSupportedVideoExt(p);
}

static bool shouldHideBrowserMediaMetadataFile(
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

static std::vector<FileEntry> listEntries(const std::filesystem::path& dir) {
  std::vector<FileEntry> entries;
  std::vector<FileEntry> items;
  std::vector<FileEntry> knownFolders;
#ifdef _WIN32
  std::filesystem::path browseDir = dir;
  if (browseDir.has_root_name() && !browseDir.has_root_directory() &&
      browseDir.relative_path().empty()) {
    std::string root = toUtf8String(browseDir.root_name());
    root.push_back('\\');
    browseDir = std::filesystem::path(root);
  }
#else
  const std::filesystem::path& browseDir = dir;
#endif

  auto appendKnownFolder = [&](const std::string& name,
                              const std::filesystem::path& path) {
    if (path.empty()) return;
    try {
      if (!std::filesystem::exists(path) ||
          !std::filesystem::is_directory(path)) {
        return;
      }
    } catch (...) {
      return;
    }
    knownFolders.push_back(FileEntry{name, path, true});
  };

  auto appendSectionHeader = [&](const std::string& name) {
    entries.push_back(FileEntry{name, std::filesystem::path(), false, true});
  };

  auto addWindowsKnownFolders = [&]() {
    std::string userProfile;
    if (const auto envProfile = getEnvString("USERPROFILE")) {
      userProfile = *envProfile;
    }
    if (userProfile.empty()) {
      const auto homeDrive = getEnvString("HOMEDRIVE");
      const auto homePath = getEnvString("HOMEPATH");
      if (homeDrive && !homeDrive->empty() && homePath && !homePath->empty()) {
        userProfile = *homeDrive + *homePath;
      } else {
        return;
      }
    }
    std::filesystem::path homePath = std::filesystem::path(userProfile);
    appendKnownFolder("Home", homePath);
    appendKnownFolder("Desktop", homePath / "Desktop");
    appendKnownFolder("Documents", homePath / "Documents");
    appendKnownFolder("Downloads", homePath / "Downloads");
    appendKnownFolder("Music", homePath / "Music");
    appendKnownFolder("Pictures", homePath / "Pictures");
    appendKnownFolder("Videos", homePath / "Videos");
  };

#ifdef _WIN32
  if (browseDir.empty()) {
    auto drives = listDriveEntries();
    if (!drives.empty()) {
      appendSectionHeader("Drives");
      for (const auto& drive : drives) {
      entries.push_back(FileEntry{drive.label, drive.path, true});
      }
    }
    addWindowsKnownFolders();
    if (!knownFolders.empty()) {
      appendSectionHeader("Locations");
      entries.insert(entries.end(), knownFolders.begin(), knownFolders.end());
    }
  } else if (browseDir == browseDir.root_path()) {
    entries.push_back(FileEntry{"..", std::filesystem::path(), true});
  }
#endif

  if (browseDir.has_parent_path() && browseDir != browseDir.root_path()) {
    entries.push_back(FileEntry{"..", browseDir.parent_path(), true});
  }

  try {
    for (const auto& entry :
         std::filesystem::directory_iterator(
             browseDir,
             std::filesystem::directory_options::skip_permission_denied)) {
      const auto& p = entry.path();
      std::error_code ec;
      if (entry.is_directory(ec) && !ec) {
        items.push_back(FileEntry{toUtf8String(p.filename()), p, true});
      } else {
        if (entry.is_regular_file(ec) && !ec && isSupportedMediaExt(p) &&
            !shouldHideBrowserMediaMetadataFile(entry)) {
          items.push_back(FileEntry{toUtf8String(p.filename()), p, false});
        }
      }
    }
  } catch (...) {
    return entries;
  }

  entries.insert(entries.end(), items.begin(), items.end());
  return entries;
}

static void refreshBrowser(BrowserState& state,
                           const std::string& initialName) {
  bool optionsActive = optionsBrowserRefresh(state);
  if (!optionsActive && isTrackBrowserActive(state)) {
    state.entries.clear();
    if (state.dir.has_parent_path()) {
      state.entries.push_back(FileEntry{"..", state.dir.parent_path(), true});
    }
    int digits = trackLabelDigits(trackBrowserTracks().size());
    for (const auto& track : trackBrowserTracks()) {
      FileEntry entry;
      entry.name = formatTrackLabel(track, digits);
      entry.path = state.dir;
      entry.isDir = false;
      entry.trackIndex = track.index;
      state.entries.push_back(std::move(entry));
    }
  } else if (!optionsActive) {
    if (trackBrowserActive() && state.dir != trackBrowserFile()) {
      clearTrackBrowserState();
    }
    state.entries = listEntries(state.dir);
  }

  if (!state.filter.empty()) {
    std::string lowFilter = toLower(state.filter);
    state.entries.erase(
        std::remove_if(state.entries.begin(), state.entries.end(),
                       [&](const FileEntry& e) {
                         if (e.isSectionHeader || e.name == "..") return false;
                         return toLower(e.name).find(lowFilter) ==
                                std::string::npos;
                       }),
        state.entries.end());
  }

  if (!state.entries.empty() && !optionsActive) {
    const bool sortingRootLevel = state.dir.empty();
#ifdef _WIN32
    auto isDriveEntry = [](const FileEntry& entry) {
      if (entry.path.empty()) return false;
      return entry.path.has_root_name() &&
             entry.path.has_root_directory() &&
             entry.path.relative_path().empty();
    };
#else
    auto isDriveEntry = [](const FileEntry&) { return false; };
#endif
    const bool hasSectionHeaders =
        std::any_of(state.entries.begin(), state.entries.end(),
                    [](const FileEntry& e) { return e.isSectionHeader; });
    auto sectionComparator = [&](const FileEntry& a, const FileEntry& b) {
      if (a.isDir != b.isDir) return a.isDir > b.isDir;
      if (sortingRootLevel && !hasSectionHeaders && isDriveEntry(a) != isDriveEntry(b)) {
        return isDriveEntry(a);
      }
      bool aLessB = false;
      if (state.sortMode == BrowserState::SortMode::Date) {
        try {
          aLessB = std::filesystem::last_write_time(a.path) <
                   std::filesystem::last_write_time(b.path);
        } catch (...) {
          aLessB = toLower(a.name) < toLower(b.name);
        }
      } else if (state.sortMode == BrowserState::SortMode::Size) {
        try {
          if (!a.isDir && !b.isDir) {
            aLessB = std::filesystem::file_size(a.path) <
                     std::filesystem::file_size(b.path);
          } else {
            aLessB = toLower(a.name) < toLower(b.name);
          }
        } catch (...) {
          aLessB = toLower(a.name) < toLower(b.name);
        }
      } else {
        aLessB = toLower(a.name) < toLower(b.name);
      }

      if (state.sortDescending) return !aLessB;
      return aLessB;
    };
    auto sortSection = [&](size_t begin, size_t end) {
      if (end <= begin + 1) return;
      std::sort(state.entries.begin() + static_cast<long long>(begin),
                state.entries.begin() + static_cast<long long>(end),
                sectionComparator);
    };

    size_t sectionStart = 0;
    if (state.entries.front().name == "..") ++sectionStart;
    while (sectionStart < state.entries.size()) {
      if (state.entries[sectionStart].isSectionHeader) {
        ++sectionStart;
        continue;
      }
      size_t sectionEnd = sectionStart;
      while (sectionEnd < state.entries.size() &&
             !state.entries[sectionEnd].isSectionHeader) {
        ++sectionEnd;
      }
      sortSection(sectionStart, sectionEnd);
      sectionStart = sectionEnd;
    }
  }

  if (state.entries.empty()) {
    state.selected = 0;
    state.scrollRow = 0;
    return;
  }

  auto isSelectable = [](const FileEntry& entry) {
    return !entry.isSectionHeader;
  };
  if (state.selected < 0 || state.selected >= static_cast<int>(state.entries.size()) ||
      !isSelectable(state.entries[static_cast<size_t>(state.selected)])) {
    state.selected = -1;
    for (size_t i = 0; i < state.entries.size(); ++i) {
      if (isSelectable(state.entries[i])) {
        state.selected = static_cast<int>(i);
        break;
      }
    }
    if (state.selected < 0) {
      state.selected = 0;
    }
  }
  state.scrollRow = 0;

  if (!initialName.empty()) {
    for (size_t i = 0; i < state.entries.size(); ++i) {
      if (state.entries[i].isSectionHeader) continue;
      if (toLower(state.entries[i].name) == toLower(initialName)) {
        state.selected = static_cast<int>(i);
        break;
      }
    }
  }
}

static std::string buildTrackSelectionMeta(const BrowserState& browser) {
  if (browser.entries.empty()) return "";
  int idx = std::clamp(browser.selected, 0,
                       static_cast<int>(browser.entries.size()) - 1);
  const auto& entry = browser.entries[static_cast<size_t>(idx)];
  std::string name = entry.name;
  if (entry.isDir && name != "..") name += "/";

  std::string sortLabel = "Name";
  if (browser.sortMode == BrowserState::SortMode::Date) sortLabel = "Date";
  else if (browser.sortMode == BrowserState::SortMode::Size) sortLabel = "Size";

  std::string dirArrow = browser.sortDescending ? " \xE2\x86\x93" : " \xE2\x86\x91";
  std::string metaLine = " [" + sortLabel + dirArrow + "]";

  metaLine += " Selected: " + name;
  if (entry.trackIndex >= 0) {
    const TrackEntry* track = findTrackEntry(entry.trackIndex);
    if (track && track->lengthMs > 0) {
      metaLine += "  " + formatTime(static_cast<double>(track->lengthMs) / 1000.0);
    }
    if (!trackBrowserTracks().empty()) {
      metaLine += "  Track " + std::to_string(entry.trackIndex + 1) + "/" +
                  std::to_string(trackBrowserTracks().size());
    }
  }
  return metaLine;
}

static bool isImageViewerEntry(const FileEntry& entry) {
  return !entry.isSectionHeader && !entry.isDir &&
         isSupportedImageExt(entry.path);
}

static std::optional<int> findImageViewerEntryIndex(
    const BrowserState& browser, const std::filesystem::path& currentFile) {
  for (int i = 0; i < static_cast<int>(browser.entries.size()); ++i) {
    const auto& entry = browser.entries[static_cast<size_t>(i)];
    if (isImageViewerEntry(entry) && entry.path == currentFile) {
      return i;
    }
  }
  return std::nullopt;
}

static std::optional<int> resolveAdjacentImageViewerEntryIndex(
    const BrowserState& browser, const std::filesystem::path& currentFile,
    int direction) {
  if (direction == 0) return std::nullopt;
  const std::optional<int> currentIndex =
      findImageViewerEntryIndex(browser, currentFile);
  if (!currentIndex) return std::nullopt;

  const int count = static_cast<int>(browser.entries.size());
  for (int idx = *currentIndex + direction; idx >= 0 && idx < count;
       idx += direction) {
    const auto& entry = browser.entries[static_cast<size_t>(idx)];
    if (isImageViewerEntry(entry)) {
      return idx;
    }
  }
  return std::nullopt;
}

static bool showAsciiArt(BrowserState& browser, const std::filesystem::path& file,
                         ConsoleInput& input, ConsoleScreen& screen,
                         const Style& baseStyle, const Style& accentStyle,
                         const Style& dimStyle,
                         bool* quitAppRequested = nullptr) {
  AsciiArt art;
  std::string error;
  std::filesystem::path currentFile = file;
  int currentIndex =
      findImageViewerEntryIndex(browser, currentFile).value_or(-1);
  int hoverIndex = -1;
  bool ok = false;
  std::vector<playback_overlay::OverlayControlSpec> controls;
  playback_overlay::OverlayCellLayout controlLayout;

  auto syncBrowserSelection = [&]() {
    if (currentIndex >= 0 &&
        currentIndex < static_cast<int>(browser.entries.size())) {
      browser.selected = currentIndex;
    }
  };

  auto makeTitle = [&]() {
    return std::string("Preview: ") + toUtf8String(currentFile.filename());
  };

  auto navigateImage = [&](int direction) -> bool {
    const std::optional<int> nextIndex =
        resolveAdjacentImageViewerEntryIndex(browser, currentFile, direction);
    if (!nextIndex) return false;
    currentIndex = *nextIndex;
    currentFile = browser.entries[static_cast<size_t>(*nextIndex)].path;
    browser.selected = *nextIndex;
    return true;
  };

  auto rebuildOverlay = [&]() {
    const int width = std::max(1, screen.width());
    const std::string title = makeTitle();

    playback_overlay::PlaybackOverlayInputs overlayInputs;
    overlayInputs.windowTitle = title;
    overlayInputs.canPlayPrevious =
        resolveAdjacentImageViewerEntryIndex(browser, currentFile, -1)
            .has_value();
    overlayInputs.canPlayNext =
        resolveAdjacentImageViewerEntryIndex(browser, currentFile, 1)
            .has_value();
    overlayInputs.overlayVisible = true;

    const playback_overlay::PlaybackOverlayState overlayState =
        playback_overlay::buildPlaybackOverlayState(overlayInputs);
    playback_overlay::OverlayControlSpecOptions controlOptions;
    controlOptions.includeRadio = false;
    controlOptions.includeAudioTrack = false;
    controlOptions.includeSubtitles = false;
    controlOptions.includePictureInPicture = false;
    const int localHoverIndex = hoverIndex >= 0 ? hoverIndex : -1;
    controls = playback_overlay::buildOverlayControlSpecs(
        overlayState, localHoverIndex, controlOptions);
    controlLayout = playback_overlay::layoutOverlayControlCells(
        playback_overlay::buildOverlayCellControlInputs(controls,
                                                        localHoverIndex),
        width);
  };

  auto renderFrame = [&]() {
    const int width = std::max(1, screen.width());
    const int height = std::max(1, screen.height());
    const std::string title = makeTitle();
    rebuildOverlay();

    const std::vector<std::string> titleLines = wrapLine(title, width);
    const int titleBottom = std::min(height, static_cast<int>(titleLines.size()));
    const int controlTop = std::max(titleBottom, height - controlLayout.height);
    const int artTop = std::clamp(titleBottom, 0, height);
    const int artBottom = std::clamp(controlTop, artTop, height);
    const int maxHeight = std::max(0, artBottom - artTop);

    error.clear();
    ok = true;
    if (maxHeight > 0) {
      ok = renderAsciiArt(currentFile, width, maxHeight, art, &error);
    }

    screen.clear(baseStyle);
    if (ok && maxHeight > 0) {
      const int artWidth = std::min(art.width, width);
      const int artHeight = std::min(art.height, maxHeight);
      const int artX = std::max(0, (width - artWidth) / 2);
      for (int y = 0; y < artHeight; ++y) {
        for (int x = 0; x < artWidth; ++x) {
          const auto& cell =
              art.cells[static_cast<size_t>(y * art.width + x)];
          Style cellStyle{cell.fg, cell.hasBg ? cell.bg : baseStyle.bg};
          screen.writeChar(artX + x, artTop + y, cell.ch, cellStyle);
        }
      }
    } else if (!error.empty()) {
      screen.writeText(0, artTop, fitLine(error, width), dimStyle);
    }

    for (size_t i = 0; i < titleLines.size(); ++i) {
      const int y = static_cast<int>(i);
      if (y < 0 || y >= height) {
        continue;
      }
      screen.writeText(0, y, fitLine(titleLines[i], width), accentStyle);
    }
    for (const auto& item : controlLayout.controls) {
      const int y = controlTop + item.y;
      if (y < 0 || y >= height || item.x >= width) continue;
      Style style = item.active ? accentStyle : dimStyle;
      if (item.hovered) {
        style = Style{style.bg, style.fg};
      }
      screen.writeText(item.x, y, fitLine(item.text, width - item.x),
                       style);
    }
    screen.draw();
  };

  auto clickOverlayControl = [&](int controlIndex) -> bool {
    if (controlIndex < 0 || controlIndex >= static_cast<int>(controls.size())) {
      return false;
    }
    playback_overlay::OverlayControlActions actions;
    actions.previous = [&]() { return navigateImage(-1); };
    actions.next = [&]() { return navigateImage(1); };
    return playback_overlay::dispatchOverlayControl(
        controls[static_cast<size_t>(controlIndex)].id, actions);
  };

  screen.updateSize();
  input.setCellPixelSize(screen.cellPixelWidth(), screen.cellPixelHeight());
  syncBrowserSelection();
  renderFrame();

  InputEvent ev{};
  while (true) {
    while (input.poll(ev)) {
      if (ev.type == InputEvent::Type::Resize) {
        screen.updateSize();
        input.setCellPixelSize(screen.cellPixelWidth(), screen.cellPixelHeight());
        renderFrame();
        continue;
      }
      if (ev.type == InputEvent::Type::Mouse) {
        const MouseEvent& mouse = ev.mouse;
        const DWORD backMask = RIGHTMOST_BUTTON_PRESSED |
                               FROM_LEFT_2ND_BUTTON_PRESSED |
                               FROM_LEFT_3RD_BUTTON_PRESSED |
                               FROM_LEFT_4TH_BUTTON_PRESSED;
        if ((mouse.buttonState & backMask) != 0) {
          return ok;
        }

        const int width = std::max(1, screen.width());
        const int height = std::max(1, screen.height());
        const std::string title = makeTitle();
        const int titleBottom =
            std::min(height, static_cast<int>(wrapLine(title, width).size()));
        const int controlTop = std::max(titleBottom, height - controlLayout.height);
        const int localControlY = mouse.pos.Y - controlTop;
        const int hitControl =
            (localControlY >= 0 && localControlY < controlLayout.height)
                ? playback_overlay::overlayCellControlAt(
                      controlLayout, mouse.pos.X, localControlY)
                : -1;
        if (mouse.eventFlags == MOUSE_MOVED) {
          if (hoverIndex != hitControl) {
            hoverIndex = hitControl;
            renderFrame();
          }
          continue;
        }

        if ((mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0 &&
            mouse.eventFlags == 0) {
          if (clickOverlayControl(hitControl)) {
            renderFrame();
            continue;
          }
        }
        continue;
      }
      if (ev.type == InputEvent::Type::Key) {
        if (auto shortcut = resolvePlaybackShortcutAction(
                ev.key, kPlaybackShortcutContextGlobal |
                            kPlaybackShortcutContextShared |
                            kPlaybackShortcutContextImageViewer)) {
          switch (*shortcut) {
            case PlaybackShortcutAction::Quit:
              if (quitAppRequested) {
                *quitAppRequested = true;
              }
              return ok;
            case PlaybackShortcutAction::CloseViewer:
              return ok;
            case PlaybackShortcutAction::Previous:
              if (navigateImage(-1)) {
                renderFrame();
              }
              continue;
            case PlaybackShortcutAction::Next:
              if (navigateImage(1)) {
                renderFrame();
              }
              continue;
            case PlaybackShortcutAction::SeekBackward:
            case PlaybackShortcutAction::SeekForward:
              continue;
            default:
              break;
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

int runTui(Options o) {
  if (o.shellOpen && !o.input.empty()) {
    std::error_code ec;
    std::filesystem::path absoluteInput =
        std::filesystem::absolute(pathFromUtf8String(o.input), ec);
    if (!ec) {
      o.input = toUtf8String(absoluteInput);
    }
  }

  configureFfmpegVideoLog({});

  AudioPlaybackConfig audioConfig;
  audioConfig.enableAudio = o.enableAudio;
  audioConfig.enableRadio = o.enableRadio;
  audioConfig.mono = o.mono;
  audioConfig.dry = o.dry;
  audioConfig.radioSettingsPath = o.radioSettingsPath;
  audioConfig.radioPresetName = o.radioPresetName;
  audioConfig.bwHz = o.bwHz;
  audioConfig.noise = o.noise;

  if (o.extractSheet) {
    return runExtractSheetCli(o, audioConfig);
  }
  if (o.splitLoop) {
    return runSplitLoopCli(o);
  }
  if (o.renderRadio) {
    return runRenderRadioCli(o);
  }

  float lpHz = static_cast<float>(o.bwHz);
  const uint32_t sampleRate = 48000;

  auto radio1938Template = std::make_unique<Radio1938>();
  radio1938Template->init(1, static_cast<float>(sampleRate), lpHz,
                          static_cast<float>(o.noise));
  if (!o.radioSettingsPath.empty()) {
    std::string radioIniError;
    if (!applyRadioSettingsIni(*radio1938Template, o.radioSettingsPath,
                               o.radioPresetName, &radioIniError)) {
      die("Failed to apply radio settings from '" + o.radioSettingsPath +
          "': " + radioIniError);
    }
  }

  std::filesystem::path startDir = radioifyLaunchDir();
  std::string initialName;
  if (!o.input.empty()) {
    std::filesystem::path inputPath = pathFromUtf8String(o.input);
    if (std::filesystem::exists(inputPath)) {
      if (std::filesystem::is_directory(inputPath)) {
        startDir = inputPath;
        o.input.clear();
      } else {
        startDir = inputPath.parent_path();
        initialName = toUtf8String(inputPath.filename());
      }
    }
  }

  BrowserState browser;
  browser.dir = startDir;
  refreshBrowser(browser, initialName);

  ConsoleInput input;
  input.init();

  ConsoleScreen screen;
  screen.init();
  input.setCellPixelSize(screen.cellPixelWidth(), screen.cellPixelHeight());

  VideoWindow tuiWindow;
  bool windowTuiEnabled = o.enableWindow;
  if (windowTuiEnabled) {
    if (!tuiWindow.Open(1280, 720, "Radioify TUI", false)) {
      windowTuiEnabled = false;
    } else {
      tuiWindow.EnableFileDrop();
      tuiWindow.SetCaptureAllMouseInput(true);
      tuiWindow.SetVsync(true);
      tuiWindow.ShowWindow(true);
    }
  }

  audioInit(audioConfig);
  PlaybackSystemControls systemControls;
  systemControls.initialize();

  VideoPlaybackConfig videoConfig;
  videoConfig.enableAscii = o.enableAscii;
  videoConfig.enableAudio = o.enableAudio;
  videoConfig.debugOverlay = o.asciiDebugOverlay;
  // If dedicated window-TUI is active, keep video playback out of the legacy
  // window path. If window-TUI could not be opened, fall back to normal
  // window playback behavior.
  videoConfig.enableWindow = o.enableWindow && !windowTuiEnabled;

  std::filesystem::path pendingImage;
  bool hasPendingImage = false;
  std::filesystem::path pendingVideo;
  bool hasPendingVideo = false;

  auto renderFile = [&](const std::filesystem::path& file) -> void {
    Options renderOpt = o;
    renderOpt.input = toUtf8String(file);
    std::filesystem::path outputPath =
        renderOpt.output.empty() ? defaultRadioOutputFor(file)
                                 : pathFromUtf8String(renderOpt.output);
    if (renderOpt.output.empty()) {
      renderOpt.output = toUtf8String(outputPath);
    }
    input.restore();
    screen.restore();
    logLine("Radioify");
    logLine(std::string("  Mode:   render"));
    logLine(std::string("  Input:  ") + toUtf8String(file));
    logLine(std::string("  Output: ") + toUtf8String(outputPath));
    logLine("Rendering output...");
    renderToFile(renderOpt, file, outputPath, *radio1938Template,
                 audioIsRadioEnabled());
    logLine("Done.");
  };

  const Color kBgBase{12, 15, 20};
  const Style kStyleNormal{{215, 220, 226}, kBgBase};
  const Style kStyleHeader{{230, 238, 248}, {18, 28, 44}};
  const Style kStyleHeaderGlow{{255, 213, 118}, {22, 34, 52}};
  const Style kStyleHeaderHot{{255, 249, 214}, {38, 50, 72}};
  const Style kStyleSearchBar{{24, 36, 66}, {160, 190, 238}};
  const Style kStyleSearchBarGlow{{18, 30, 60}, {178, 206, 246}};
  const Style kStyleSearchBarActive{{14, 24, 50}, {196, 220, 248}};
  const Style kStyleAccent{{255, 214, 120}, kBgBase};
  const Style kStyleDim{{138, 144, 153}, kBgBase};
  const Style kStyleAlert{{255, 92, 92}, kBgBase};
  const Style kStyleDir{{110, 231, 183}, kBgBase};
  const Style kStyleHighlight{{15, 20, 28}, {230, 238, 248}};
  const Style kStyleBreadcrumbHover{{15, 20, 28}, {255, 214, 120}};
  const Style kStyleActionActive{{15, 20, 28}, {255, 214, 120}};
  const Color kProgressStart{110, 231, 183};
  const Color kProgressEnd{255, 214, 110};
  const Style kStyleProgressEmpty{{32, 38, 46}, {32, 38, 46}};
  const Style kStyleProgressFrame{{160, 170, 182}, kBgBase};

  auto showPlaybackErrorDialog = [&](const std::filesystem::path& file) {
    std::string error = audioGetWarning();
    if (error.empty()) {
      error = "Failed to start playback.";
    }

    std::string title = "Playback Error";
    std::string message = error;
    std::string detail = toUtf8String(file.filename());
    std::string ext = toLower(toUtf8String(file.extension()));
    if ((ext == ".psf2" || ext == ".minipsf2") &&
        error.find("hebios.bin") != std::string::npos) {
      title = "PSF2 BIOS Required";
      message = "Missing hebios.bin for PSF2 playback.";
      detail =
          "Set RADIOIFY_PSF_BIOS or place hebios.bin next to the file, next "
          "to radioify.exe, or in Radioify's launch directory.";
    }

    playback_dialog::showInfoDialog(
        input, screen, kStyleNormal, kStyleAccent, kStyleDim, title, message,
        detail, "Enter/Space/Esc: close");
  };

  auto tryStartAudioFile = [&](const std::filesystem::path& file,
                               int trackIndex = 0) {
    if (audioStartFile(file, trackIndex)) {
      return true;
    }
    showPlaybackErrorDialog(file);
    return false;
  };

  bool dirty = true;
  if (!o.input.empty() && o.play) {
    std::filesystem::path inputPath = pathFromUtf8String(o.input);
    if (std::filesystem::exists(inputPath)) {
      if (!std::filesystem::is_directory(inputPath)) {
        if (isSupportedImageExt(inputPath)) {
          pendingImage = inputPath;
          hasPendingImage = true;
        } else if (isVideoExt(inputPath)) {
          pendingVideo = inputPath;
          hasPendingVideo = true;
        } else {
          if (loadTrackBrowserForFile(inputPath)) {
            browser.dir = trackBrowserFile();
            browser.selected = 0;
            browser.scrollRow = 0;
            browser.filter.clear();
            setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
            refreshBrowser(browser, "");
          } else {
            tryStartAudioFile(inputPath);
          }
        }
      }
    }
  }

  screen.clear(kStyleNormal);
  screen.draw();

  bool running = true;
  auto lastDraw = std::chrono::steady_clock::now();
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
  ActionStripLayout actionStrip;
  int actionHover = -1;
  int breadcrumbHover = -1;
  bool searchBarHover = false;
  bool searchBarClearHover = false;
  const int searchBarClearButtonWidth = 5;
  int headerLines = 0;
  int listTop = 0;
  int breadcrumbY = 0;
  int searchBarY = -1;
  int searchBarWidth = 0;
  int searchBarClearStart = -1;
  int searchBarClearEnd = -1;
  int width = 0;
  int height = 0;
  int listHeight = 0;
  GridLayout layout;
  BreadcrumbLine breadcrumbLine;
  bool didRender = false;
  bool melodyVisualizationEnabled = false;
  std::deque<int> melodyHistoryMidi;
  std::deque<float> melodyHistoryConfidence;
  constexpr size_t kMelodyHistoryMaxSamples = 4096;
  std::filesystem::path lastMelodyTrack;
  int lastMelodyTrackIndex = -1;
  bool lastMelodyAnalysisRunning = false;
  double seekDisplaySec = -1.0;
  bool seekHoldActive = false;
  auto seekHoldStart = std::chrono::steady_clock::now();
  std::vector<ScreenCell> windowCells;
  AudioMiniPlayer audioMiniPlayer;
  ConsoleInputPump consoleInputPump;
  UiDirtyFlags dirtyFlags = UiDirtyFlags::Frame | UiDirtyFlags::Layout;
  bool layoutDirty = true;
  bool forceFullRedraw = true;
  bool screenSizeDirty = true;
  BrowserViewport viewport;
  BrowserFooterLayout footerLayout;
  auto markDirty = [&](UiDirtyFlags flags = UiDirtyFlags::Frame) {
    dirty = true;
    dirtyFlags |= flags;
    if (hasDirtyFlag(flags, UiDirtyFlags::Layout)) {
      layoutDirty = true;
      forceFullRedraw = true;
    }
  };
  auto markLayoutDirty = [&]() { markDirty(UiDirtyFlags::Frame | UiDirtyFlags::Layout); };
  auto markSeekHold = [&](double targetSec) {
    if (!std::isfinite(targetSec) || targetSec < 0.0) return;
    seekDisplaySec = targetSec;
    seekHoldActive = true;
    seekHoldStart = std::chrono::steady_clock::now();
  };
  auto midiToNoteName = [](int midi) {
    if (midi < 0 || midi > 127) return std::string("??");
    static const char* kNoteNames[] = {"C",  "C#", "D",  "D#", "E",  "F",
                                      "F#", "G",  "G#", "A",  "A#", "B"};
    int note = midi % 12;
    if (note < 0) note += 12;
    int octave = midi / 12 - 1;
    return std::string(kNoteNames[static_cast<size_t>(note)]) +
           std::to_string(octave);
  };
  auto clearMelodyHistory = [&]() {
    melodyHistoryMidi.clear();
    melodyHistoryConfidence.clear();
  };
  auto pushMelodyHistory = [&](const AudioMelodyInfo& info) {
    int midi = (info.midiNote >= 0) ? info.midiNote : -1;
    melodyHistoryMidi.push_back(midi);
    melodyHistoryConfidence.push_back(std::clamp(info.confidence, 0.0f, 1.0f));
    while (melodyHistoryMidi.size() > kMelodyHistoryMaxSamples) {
      melodyHistoryMidi.pop_front();
      melodyHistoryConfidence.pop_front();
    }
  };
  auto buildAudioNowPlayingLabel = [&]() {
    std::filesystem::path nowPlaying = audioGetNowPlaying();
    std::string label =
        nowPlaying.empty() ? std::string("(none)")
                           : toUtf8String(nowPlaying.filename());
    int trackIndex = audioGetTrackIndex();
    if (!nowPlaying.empty() && trackIndex >= 0) {
      int digits = 3;
      const TrackEntry* track = nullptr;
      TrackEntry fallback{};
      if (nowPlaying == trackBrowserFile() && !trackBrowserTracks().empty()) {
        digits = trackLabelDigits(trackBrowserTracks().size());
        track = findTrackEntry(trackIndex);
      }
      if (!track) {
        fallback.index = trackIndex;
        track = &fallback;
      }
      label += "  |  " + formatTrackLabel(*track, digits);
    }
    return label;
  };

  playback_transport_navigation::Navigator::Callbacks transportCallbacks;
  transportCallbacks.dirty = &dirty;
  transportCallbacks.markDirty = [&]() { markDirty(); };
  transportCallbacks.markLayoutDirty = [&]() { markLayoutDirty(); };
  transportCallbacks.refreshBrowser = [&](const std::string& initialName) {
    refreshBrowser(browser, initialName);
  };
  playback_transport_navigation::Navigator transportNavigator(
      browser, std::move(transportCallbacks));

  std::function<bool(const PlaybackTarget&)> playPlaybackTarget;
  playPlaybackTarget = [&](const PlaybackTarget& initialTarget) {
    PlaybackTarget target = initialTarget;
    PlaybackSessionContinuationState playbackContinuationState;
    while (!target.file.empty()) {
      if (!transportNavigator.syncBrowserToPlaybackTarget(target)) {
        return false;
      }
      if (target.trackIndex >= 0) {
        return tryStartAudioFile(target.file, target.trackIndex);
      }
      if (isSupportedImageExt(target.file)) {
        bool quitAppRequested = false;
        showAsciiArt(browser, target.file, input, screen, kStyleNormal,
                     kStyleAccent, kStyleDim, &quitAppRequested);
        if (quitAppRequested) {
          running = false;
          return true;
        }
        markDirty();
        return true;
      }
      if (isVideoExt(target.file)) {
        bool quitAppRequested = false;
        std::optional<PlaybackTarget> pendingTransportTarget;
        auto requestTransportCommand = [&](PlaybackTransportCommand command) {
          const int direction =
              (command == PlaybackTransportCommand::Previous) ? -1 : 1;
          pendingTransportTarget =
              transportNavigator.resolveAdjacentPlaybackTarget(target,
                                                              direction);
          return pendingTransportTarget.has_value();
        };
        auto requestOpenFiles =
            [&](const std::vector<std::filesystem::path>& files) {
          if (auto droppedTarget =
                  playback_target_resolver::resolveDroppedTarget(files)) {
            pendingTransportTarget = *droppedTarget;
            return true;
          }
          return false;
        };
        bool handled = showAsciiVideo(
            target.file, input, screen, kStyleNormal, kStyleAccent, kStyleDim,
            kStyleProgressEmpty, kStyleProgressFrame, kProgressStart,
            kProgressEnd, videoConfig, &quitAppRequested, &systemControls,
            requestTransportCommand, requestOpenFiles,
            &playbackContinuationState);
        if (quitAppRequested) {
          running = false;
          return true;
        }
        if (pendingTransportTarget) {
          target = *pendingTransportTarget;
          continue;
        }
        if (handled) {
          markDirty();
          return true;
        }
        return tryStartAudioFile(target.file);
      }
      return tryStartAudioFile(target.file);
    }
    return false;
  };

  if (hasPendingImage) {
    playPlaybackTarget({pendingImage, -1});
  }
  if (hasPendingVideo) {
    playPlaybackTarget({pendingVideo, -1});
  }

  struct CommandEntry {
    std::string label;
    std::string hotkey;
    bool enabled = true;
    std::function<void()> run;
  };

  struct FileContextMenuState {
    bool active = false;
    FileEntry entry{};
    int selected = 0;
    int anchorX = -1;
    int anchorY = -1;
  };

  struct FileContextMenuLayout {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int listY = 0;
    int rows = 0;
    bool valid = false;
  };

  struct MelodyExportTaskState {
    std::mutex mutex;
    std::thread worker;
    bool running = false;
    bool hasResult = false;
    bool success = false;
    float progress = 0.0f;
    std::string status;
    std::filesystem::path sourceFile;
    std::filesystem::path outputFile;
  };

  struct PaletteLayout {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    int innerWidth = 0;
    int inputY = 0;
    int listY = 0;
    int listRows = 0;
    bool valid = false;
  };

  FileContextMenuState fileContextMenu;
  FileContextMenuLayout fileContextLayout;
  MelodyExportTaskState melodyExportTask;
  LoopSplitTaskState loopSplitTask;

  struct ActionRenderItem {
    ActionStripItem id;
    std::string label;
    std::string labelHover;
    bool active;
    int width;
  };

  auto buildActionRenderItems = [&](bool browserInteractionEnabled) {
    std::vector<ActionRenderItem> items;
    auto addActionItem = [&](ActionStripItem id, const std::string& text,
                             bool active) {
      BracketButtonLabels labels = makeBracketButtonLabels(text);
      items.push_back(
          {id, labels.normal, labels.hover, active, labels.width});
    };
    auto actionStripItemForOverlayControl =
        [](playback_overlay::OverlayControlId id)
        -> std::optional<ActionStripItem> {
          switch (id) {
            case playback_overlay::OverlayControlId::Previous:
              return ActionStripItem::Previous;
            case playback_overlay::OverlayControlId::PlayPause:
              return ActionStripItem::PlayPause;
            case playback_overlay::OverlayControlId::Next:
              return ActionStripItem::Next;
            case playback_overlay::OverlayControlId::Radio:
              return ActionStripItem::Radio;
            case playback_overlay::OverlayControlId::Hz50:
              return ActionStripItem::Hz50;
            case playback_overlay::OverlayControlId::PictureInPicture:
              return ActionStripItem::PictureInPicture;
            case playback_overlay::OverlayControlId::AudioTrack:
            case playback_overlay::OverlayControlId::Subtitles:
              return std::nullopt;
          }
          return std::nullopt;
        };

    playback_overlay::PlaybackOverlayState actionOverlayState;
    const std::filesystem::path nowPlaying = audioGetNowPlaying();
    actionOverlayState.audioOk = audioIsReady();
    actionOverlayState.playPauseAvailable =
        actionOverlayState.audioOk && !audioIsFinished();
    actionOverlayState.audioSupports50HzToggle =
        actionOverlayState.audioOk && audioSupports50HzToggle();
    actionOverlayState.canPlayPrevious =
        actionOverlayState.audioOk || !nowPlaying.empty();
    actionOverlayState.canPlayNext =
        actionOverlayState.audioOk || !nowPlaying.empty();
    actionOverlayState.radioEnabled = audioIsRadioEnabled();
    actionOverlayState.hz50Enabled = audioIs50HzEnabled();
    actionOverlayState.paused = audioIsPaused();
    actionOverlayState.audioFinished = audioIsFinished();
    actionOverlayState.pictureInPictureAvailable =
        audioMiniPlayer.isOpen() || actionOverlayState.audioOk ||
        !nowPlaying.empty();
    actionOverlayState.pictureInPictureActive = audioMiniPlayer.isOpen();
    playback_overlay::OverlayControlSpecOptions controlOptions;
    controlOptions.includeAudioTrack = false;
    controlOptions.includeSubtitles = false;
    std::vector<playback_overlay::OverlayControlSpec> controlSpecs =
        playback_overlay::buildOverlayControlSpecs(actionOverlayState, -1,
                                                   controlOptions);
    for (const playback_overlay::OverlayControlSpec& spec : controlSpecs) {
      std::optional<ActionStripItem> actionId =
          actionStripItemForOverlayControl(spec.id);
      if (actionId) {
        items.push_back({*actionId, spec.normalText, spec.hoverText,
                         spec.active, spec.width});
      }
    }

    if (browserInteractionEnabled) {
      const std::string gridIcon = "\xE2\x96\xA6";
      const std::string listIcon = "\xE2\x89\xA1";
      const std::string previewIcon = "\xE2\x98\x90";
      std::string viewState;
      switch (browser.viewMode) {
        case BrowserState::ViewMode::Thumbnails:
          viewState = gridIcon + " Grid";
          break;
        case BrowserState::ViewMode::ListOnly:
          viewState = listIcon + " List";
          break;
        case BrowserState::ViewMode::ListPreview:
          viewState = previewIcon + " Preview";
          break;
      }
      addActionItem(ActionStripItem::View, viewState, false);
    }
    if (browserInteractionEnabled && optionsBrowserCanToggle(browser)) {
      addActionItem(ActionStripItem::Options, "Options",
                    optionsBrowserIsActive(browser));
    }
    return items;
  };

  auto countWrappedActionLines =
      [](const std::vector<ActionRenderItem>& items, int width) {
        if (items.empty() || width <= 0) return 0;
        const int gapWidth = 2;
        int lines = 1;
        int x = 0;
        for (const auto& item : items) {
          const int itemWidth = std::min(std::max(1, item.width), width);
          const int gap = x > 0 ? gapWidth : 0;
          if (x > 0 && x + gap + itemWidth > width) {
            ++lines;
            x = itemWidth;
          } else {
            x += gap + itemWidth;
          }
        }
        return lines;
      };

  auto buildFooterLayout = [&]() {
    bool hasAnalyzeStatus = false;
    {
      std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
      hasAnalyzeStatus =
          melodyExportTask.hasResult && !melodyExportTask.status.empty();
    }
    bool hasLoopSplitStatus = false;
    {
      std::lock_guard<std::mutex> lock(loopSplitTask.mutex);
      hasLoopSplitStatus =
          loopSplitTask.hasResult && !loopSplitTask.status.empty();
    }
    const std::filesystem::path nowPlaying = audioGetNowPlaying();
    const bool showNowPlaying =
        !nowPlaying.empty() || audioIsReady() || audioIsSeeking() ||
        audioIsHolding();
    BrowserFooterLayout layout = computeBrowserFooterLayout(
        !melodyVisualizationEnabled, !audioGetWarning().empty(),
        hasAnalyzeStatus, hasLoopSplitStatus, o.play, showNowPlaying,
        o.play && audioIsReady());
    if (layout.showNowPlaying) {
      const int nowPlayingLines = std::max(
          1, wrappedLineCount(" " + buildAudioNowPlayingLabel(),
                              screen.width()));
      layout.reservedLines += nowPlayingLines - layout.nowPlayingLines;
      layout.nowPlayingLines = nowPlayingLines;
    }
    if (layout.showActionStrip) {
      const bool browserInteractionEnabled = !melodyVisualizationEnabled;
      layout.actionStripLines = countWrappedActionLines(
          buildActionRenderItems(browserInteractionEnabled), screen.width());
      layout.reservedLines += std::max(0, layout.actionStripLines - 1);
    }
    return layout;
  };

  auto rebuildLayout = [&]() {
    if (screenSizeDirty) {
      screen.updateSize();
      input.setCellPixelSize(screen.cellPixelWidth(), screen.cellPixelHeight());
      screenSizeDirty = false;
    }
    footerLayout = buildFooterLayout();
    const bool browserInteractionEnabled = !melodyVisualizationEnabled;
    const bool showHeaderLabel =
        browserInteractionEnabled &&
        (optionsBrowserIsActive(browser) || isTrackBrowserActive(browser));
    viewport = computeBrowserViewport(screen.width(), screen.height(),
                                      browserInteractionEnabled,
                                      showHeaderLabel,
                                      footerLayout.reservedLines,
                                      searchBarClearButtonWidth);
    width = viewport.width;
    height = viewport.height;
    headerLines = viewport.headerLines;
    searchBarY = viewport.searchBarY;
    searchBarWidth = viewport.searchBarWidth;
    searchBarClearStart = viewport.searchBarClearStart;
    searchBarClearEnd = viewport.searchBarClearEnd;
    breadcrumbY = viewport.breadcrumbY;
    listTop = viewport.listTop;
    if (browserInteractionEnabled) {
      listTop = std::max(listTop, breadcrumbY + 1);
    }
    listHeight = std::max(1, height - listTop - footerLayout.reservedLines);
    layout = buildLayout(browser, width, listHeight);
    if (layout.totalRows <= layout.rowsVisible) {
      browser.scrollRow = 0;
    } else {
      int maxScroll = layout.totalRows - layout.rowsVisible;
      browser.scrollRow = std::clamp(browser.scrollRow, 0, maxScroll);
    }
    breadcrumbLine = buildBreadcrumbLine(browser.dir, width);
    if (!browserInteractionEnabled) {
      breadcrumbHover = -1;
    } else if (breadcrumbHover >= static_cast<int>(breadcrumbLine.crumbs.size())) {
      breadcrumbHover = -1;
    }
    layoutDirty = false;
  };

  auto isBackgroundTaskRunning = [&]() {
    bool running = false;
    {
      std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
      running = running || melodyExportTask.running;
    }
    {
      std::lock_guard<std::mutex> lock(loopSplitTask.mutex);
      running = running || loopSplitTask.running;
    }
    return running;
  };

  InputCallbacks callbacks;
  callbacks.onQuit = [&]() { running = false; };
  callbacks.onRefreshBrowser = [&](BrowserState& nextBrowser,
                                   const std::string& initialName) {
    refreshBrowser(nextBrowser, initialName);
    markLayoutDirty();
  };
  callbacks.onPlayFile = [&](const std::filesystem::path& file) {
    OptionsBrowserResult optionsResult =
        optionsBrowserActivateSelection(browser);
    if (optionsResult == OptionsBrowserResult::Changed) {
      refreshBrowser(browser, "");
      markLayoutDirty();
      return true;
    }
    if (optionsResult == OptionsBrowserResult::Handled) {
      return true;
    }
    if (isTrackBrowserActive(browser)) {
      if (!browser.entries.empty()) {
        int idx = std::clamp(browser.selected, 0,
                             static_cast<int>(browser.entries.size()) - 1);
        const auto& entry = browser.entries[static_cast<size_t>(idx)];
        if (entry.trackIndex >= 0) {
          return playPlaybackTarget({file, entry.trackIndex});
        }
      }
      return true;
    }
    if (isSupportedImageExt(file) || isVideoExt(file)) {
      return playPlaybackTarget({file, -1});
    }
    if (transportNavigator.activateTrackBrowser(file)) {
      return true;
    }
    return tryStartAudioFile(file);
  };
  callbacks.onPlayFiles =
      [&](const std::vector<std::filesystem::path>& files) {
    if (auto droppedTarget =
            playback_target_resolver::resolveDroppedTarget(files)) {
      return playPlaybackTarget(*droppedTarget);
    }
    return false;
  };
  callbacks.onOpenFileContextMenu = [&](const FileEntry& entry, int x, int y) {
    if (!o.play || entry.isDir || !isSupportedAudioExt(entry.path)) {
      return;
    }
    fileContextMenu.active = true;
    fileContextMenu.entry = entry;
    fileContextMenu.selected = 0;
    fileContextMenu.anchorX = x;
    fileContextMenu.anchorY = y;
    markDirty();
  };
  callbacks.onRenderFile = [&](const std::filesystem::path& file) {
    renderFile(file);
    didRender = true;
  };
  callbacks.onPlay = [&]() {
    const std::filesystem::path nowPlaying = audioGetNowPlaying();
    if (nowPlaying.empty()) {
      return;
    }
    if (audioIsFinished()) {
      playPlaybackTarget({nowPlaying, audioGetTrackIndex()});
      return;
    }
    if (audioIsReady() && audioIsPaused()) {
      audioTogglePause();
      markDirty();
    }
  };
  callbacks.onPause = [&]() {
    if (audioIsReady() && !audioIsPaused()) {
      audioTogglePause();
      markDirty();
    }
  };
  callbacks.onTogglePause = [&]() {
    const std::filesystem::path nowPlaying = audioGetNowPlaying();
    if (nowPlaying.empty()) {
      return;
    }
    if (audioIsFinished()) {
      playPlaybackTarget({nowPlaying, audioGetTrackIndex()});
      return;
    }
    audioTogglePause();
    markDirty();
  };
  callbacks.onStopPlayback = [&]() {
    if (audioIsReady()) {
      audioStop();
      markDirty();
    }
  };
  callbacks.onCurrentPlaybackFile = [&]() { return audioGetNowPlaying(); };
  callbacks.onPlayPrevious = [&]() {
    PlaybackTarget current{audioGetNowPlaying(), audioGetTrackIndex()};
    if (current.file.empty()) {
      return;
    }
    if (auto target =
            transportNavigator.resolveAdjacentPlaybackTarget(current, -1)) {
      playPlaybackTarget(*target);
    }
  };
  callbacks.onPlayNext = [&]() {
    PlaybackTarget current{audioGetNowPlaying(), audioGetTrackIndex()};
    if (current.file.empty()) {
      return;
    }
    if (auto target =
            transportNavigator.resolveAdjacentPlaybackTarget(current, 1)) {
      playPlaybackTarget(*target);
    }
  };
  callbacks.onToggleRadio = [&]() {
    audioToggleRadio();
    markDirty();
  };
  callbacks.onToggle50Hz = [&]() {
    if (audioSupports50HzToggle()) {
      audioToggle50Hz();
      markDirty();
    }
  };
  callbacks.onToggleOptions = [&]() {
    if (optionsBrowserCanToggle(browser)) {
      optionsBrowserToggle(browser);
      refreshBrowser(browser, "");
      markLayoutDirty();
    }
  };
  callbacks.onSeekBy = [&](int direction) {
    audioSeekBy(direction);
    markSeekHold(audioGetSeekTargetSec());
    markDirty();
  };
  callbacks.onSeekToRatio = [&](double ratio) {
    audioSeekToRatio(ratio);
    markSeekHold(audioGetSeekTargetSec());
    markDirty();
  };
  callbacks.onAdjustVolume = [&](float delta) {
    audioAdjustVolume(delta);
    markDirty();
  };
  callbacks.onToggleWindow = [&]() {
    if (!audioMiniPlayer.isOpen() && audioGetNowPlaying().empty() &&
        !audioIsReady()) {
      return;
    }
    if (audioMiniPlayer.toggle()) {
      markDirty(UiDirtyFlags::Async);
    }
  };

  AudioMiniPlayer::Styles audioMiniStyles{kStyleNormal,
                                          kStyleAccent,
                                          kStyleDim,
                                          kStyleAlert,
                                          kStyleActionActive,
                                          kStyleProgressEmpty,
                                          kStyleProgressFrame,
                                          kProgressStart,
                                          kProgressEnd};
  auto buildAudioMiniContext = [&]() {
    AudioMiniPlayer::Context context;
    context.nowPlayingLabel = buildAudioNowPlayingLabel();
    context.nowPlayingPath = audioGetNowPlaying();
    context.trackIndex = audioGetTrackIndex();
    return context;
  };
  auto renderAudioMiniPlayer = [&]() {
    if (!audioMiniPlayer.isOpen()) {
      return;
    }
    audioMiniPlayer.render(audioMiniStyles, buildAudioMiniContext());
  };
  AudioMiniPlayer::Callbacks audioMiniCallbacks;
  audioMiniCallbacks.onQuit = [&]() {
    if (callbacks.onQuit) callbacks.onQuit();
  };
  audioMiniCallbacks.onTogglePause = [&]() {
    if (callbacks.onTogglePause) callbacks.onTogglePause();
  };
  audioMiniCallbacks.onStopPlayback = [&]() {
    if (callbacks.onStopPlayback) callbacks.onStopPlayback();
  };
  audioMiniCallbacks.onPlayPrevious = [&]() {
    if (callbacks.onPlayPrevious) callbacks.onPlayPrevious();
  };
  audioMiniCallbacks.onPlayNext = [&]() {
    if (callbacks.onPlayNext) callbacks.onPlayNext();
  };
  audioMiniCallbacks.onToggleRadio = [&]() {
    if (callbacks.onToggleRadio) callbacks.onToggleRadio();
  };
  audioMiniCallbacks.onToggle50Hz = [&]() {
    if (callbacks.onToggle50Hz) callbacks.onToggle50Hz();
  };
  audioMiniCallbacks.onSeekBy = [&](int direction) {
    if (callbacks.onSeekBy) callbacks.onSeekBy(direction);
  };
  audioMiniCallbacks.onSeekToRatio = [&](double ratio) {
    if (callbacks.onSeekToRatio) callbacks.onSeekToRatio(ratio);
  };
  audioMiniCallbacks.onAdjustVolume = [&](float delta) {
    if (callbacks.onAdjustVolume) callbacks.onAdjustVolume(delta);
  };
  audioMiniCallbacks.onPlayFiles =
      [&](const std::vector<std::filesystem::path>& files) {
    if (!callbacks.onPlayFiles) {
      return false;
    }
    return callbacks.onPlayFiles(files);
  };
  audioMiniCallbacks.onClose = [&]() { markDirty(UiDirtyFlags::Async); };

  auto handleSystemPlaybackCommand = [&](PlaybackControlCommand command) {
    switch (command) {
      case PlaybackControlCommand::Play:
        if (callbacks.onPlay) callbacks.onPlay();
        break;
      case PlaybackControlCommand::Pause:
        if (callbacks.onPause) callbacks.onPause();
        break;
      case PlaybackControlCommand::TogglePause:
        if (callbacks.onTogglePause) callbacks.onTogglePause();
        break;
      case PlaybackControlCommand::Stop:
        if (callbacks.onStopPlayback) callbacks.onStopPlayback();
        break;
      case PlaybackControlCommand::Previous:
        if (callbacks.onPlayPrevious) callbacks.onPlayPrevious();
        break;
      case PlaybackControlCommand::Next:
        if (callbacks.onPlayNext) callbacks.onPlayNext();
        break;
    }
  };

  auto syncAudioSystemControls = [&]() {
    std::filesystem::path nowPlaying = audioGetNowPlaying();
    if (nowPlaying.empty()) {
      systemControls.clear();
      return;
    }

    PlaybackSystemControls::State state;
    state.active = true;
    state.isVideo = false;
    state.file = nowPlaying;
    state.trackIndex = audioGetTrackIndex();
    state.positionSec = audioGetTimeSec();
    state.durationSec = audioGetTotalSec();
    state.canPlay = true;
    state.canPause = true;
    state.canStop = true;
    state.canPrevious = true;
    state.canNext = true;
    if (audioIsFinished()) {
      state.status = PlaybackSystemControls::Status::Stopped;
    } else if (audioIsPaused()) {
      state.status = PlaybackSystemControls::Status::Paused;
    } else {
      state.status = PlaybackSystemControls::Status::Playing;
    }
    systemControls.update(state);
  };

  auto processSystemPlaybackCommands = [&]() {
    PlaybackControlCommand command;
    while (systemControls.pollCommand(&command)) {
      handleSystemPlaybackCommand(command);
    }
  };
  callbacks.onResize = [&]() {
    screenSizeDirty = true;
    markLayoutDirty();
  };

  bool paletteActive = false;
  std::string paletteQuery;
  int paletteSelected = 0;
  int paletteScroll = 0;
  std::vector<int> paletteFiltered;
  PaletteLayout paletteLayout;

  auto buildCommands = [&]() {
    std::vector<CommandEntry> cmds;
    cmds.push_back({"Play/Pause", "Space", true, [&]() {
                      audioTogglePause();
                      markDirty();
                    }});
    if (audioMiniPlayer.isOpen() || !audioGetNowPlaying().empty() ||
        audioIsReady()) {
      cmds.push_back({"PiP Mini Player", "W", true, [&]() {
                        if (callbacks.onToggleWindow) callbacks.onToggleWindow();
                      }});
    }
    cmds.push_back({"Radio Filter",
                    "R", true, [&]() {
                      audioToggleRadio();
                      markDirty();
                    }});
    bool show50Hz = audioSupports50HzToggle();
    if (show50Hz) {
      cmds.push_back({"50Hz",
                      "H", true, [&]() {
                        audioToggle50Hz();
                        markDirty();
                      }});
    }
    if (!melodyVisualizationEnabled) {
      cmds.push_back({"View: Grid", "T", true, [&]() {
                        browser.viewMode = BrowserState::ViewMode::Thumbnails;
                        markLayoutDirty();
                      }});
      cmds.push_back({"View: List", "T", true, [&]() {
                        browser.viewMode = BrowserState::ViewMode::ListOnly;
                        markLayoutDirty();
                      }});
      cmds.push_back({"View: Preview", "T", true, [&]() {
                        browser.viewMode = BrowserState::ViewMode::ListPreview;
                        markLayoutDirty();
                      }});
      if (optionsBrowserCanToggle(browser)) {
        cmds.push_back({"Options", "O", true, [&]() {
                          optionsBrowserToggle(browser);
                          refreshBrowser(browser, "");
                          markLayoutDirty();
                        }});
      }
    }
    cmds.push_back({"Quit", "Q", true, [&]() {
                      if (callbacks.onQuit) callbacks.onQuit();
                    }});
    return cmds;
  };

  auto filterCommands = [&](const std::vector<CommandEntry>& cmds,
                            std::vector<int>* out) {
    out->clear();
    std::string q = toLower(paletteQuery);
    auto fuzzyMatch = [](const std::string& text,
                         const std::string& query) {
      if (query.empty()) return true;
      size_t ti = 0;
      for (char qc : query) {
        ti = text.find(qc, ti);
        if (ti == std::string::npos) return false;
        ++ti;
      }
      return true;
    };
    for (int i = 0; i < static_cast<int>(cmds.size()); ++i) {
      if (!cmds[static_cast<size_t>(i)].enabled) continue;
      if (q.empty()) {
        out->push_back(i);
        continue;
      }
      std::string labelLower = toLower(cmds[static_cast<size_t>(i)].label);
      if (fuzzyMatch(labelLower, q)) {
        out->push_back(i);
      }
    }
  };

  auto computePaletteLayout = [&](int w, int h, int topInset, int listRows) {
    PaletteLayout layout{};
    const int minTop = std::clamp(topInset, 1, std::max(1, h - 1));
    const int availableHeight = std::max(1, h - minTop);
    int maxWidth = std::max(30, w - 4);
    layout.width = std::min(72, maxWidth);
    layout.innerWidth = std::max(1, layout.width - 2);
    layout.listRows = std::max(1, listRows);
    layout.height = std::min(layout.listRows + 3, availableHeight);
    layout.listRows = std::max(1, layout.height - 3);
    layout.x = std::max(0, (w - layout.width) / 2);
    layout.y =
        minTop + std::max(0, (availableHeight - layout.height) / 2);
    layout.inputY = layout.y + 1;
    layout.listY = layout.y + 2;
    layout.valid = true;
    return layout;
  };

  auto ensurePaletteScroll = [&](int count, int visibleRows) {
    if (count <= 0) {
      paletteSelected = 0;
      paletteScroll = 0;
      return;
    }
    paletteSelected =
        std::clamp(paletteSelected, 0, std::max(0, count - 1));
    if (paletteSelected < paletteScroll) {
      paletteScroll = paletteSelected;
    } else if (paletteSelected >= paletteScroll + visibleRows) {
      paletteScroll = paletteSelected - visibleRows + 1;
    }
    paletteScroll =
        std::clamp(paletteScroll, 0, std::max(0, count - visibleRows));
  };

  auto buildMelodyOutputPath = [&](const FileEntry& entry) {
    std::filesystem::path output = entry.path;
    int trackIndex = entry.trackIndex;
    if (trackIndex >= 0) {
      char suffix[32];
      std::snprintf(suffix, sizeof(suffix), ".track%03d.melody", trackIndex);
      output += suffix;
      return output;
    }
    output.replace_extension(".melody");
    return output;
  };

  auto cleanupMelodyExportWorker = [&]() {
    bool shouldJoin = false;
    {
      std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
      shouldJoin =
          !melodyExportTask.running && melodyExportTask.worker.joinable();
    }
    if (shouldJoin) {
      melodyExportTask.worker.join();
    }
  };

  auto joinMelodyExportWorker = [&]() {
    if (melodyExportTask.worker.joinable()) {
      melodyExportTask.worker.join();
    }
  };

  auto startMelodyExport = [&](const FileEntry& entry) {
    if (entry.isDir || !isSupportedAudioExt(entry.path)) {
      return;
    }
    if (isBackgroundTaskRunning()) {
      return;
    }

    cleanupMelodyExportWorker();

    int trackIndex = entry.trackIndex >= 0 ? entry.trackIndex : 0;
    std::filesystem::path outputFile = buildMelodyOutputPath(entry);

    {
      std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
      if (melodyExportTask.running) {
        return;
      }
      melodyExportTask.running = true;
      melodyExportTask.hasResult = false;
      melodyExportTask.success = false;
      melodyExportTask.progress = 0.0f;
      melodyExportTask.status.clear();
      melodyExportTask.sourceFile = entry.path;
      melodyExportTask.outputFile = outputFile;
    }

    melodyExportTask.worker =
        std::thread([entryPath = entry.path, trackIndex, outputFile,
                     &melodyExportTask]() {
          auto onProgress = [&melodyExportTask](float progress) {
            std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
            melodyExportTask.progress = std::clamp(progress, 0.0f, 1.0f);
          };
          std::string error;
          bool ok = audioAnalyzeFileToMelodyFile(entryPath, trackIndex,
                                                 outputFile, onProgress, &error);
          std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
          melodyExportTask.running = false;
          melodyExportTask.hasResult = true;
          melodyExportTask.success = ok;
          melodyExportTask.progress = ok ? 1.0f : melodyExportTask.progress;
          if (ok) {
            std::filesystem::path midiOutput = outputFile;
            midiOutput.replace_extension(".mid");
            melodyExportTask.status =
                "Saved " + toUtf8String(outputFile.filename()) + " and " +
                toUtf8String(midiOutput.filename());
          } else {
            melodyExportTask.status =
                error.empty() ? "Analysis failed." : error;
          }
        });
  };

  auto computeFileContextLayout = [&](int w, int h, int topInset) {
    FileContextMenuLayout layout{};
    std::vector<std::string> items = {" Play", " Analyze", " Split Loop"};
    int itemWidth = 0;
    for (const auto& item : items) {
      itemWidth = std::max(itemWidth, utf8DisplayWidth(item));
    }
    layout.width = std::max(24, itemWidth + 4);
    layout.height = static_cast<int>(items.size()) + 2;
    const int minTop = std::clamp(topInset, 1, std::max(1, h - 1));
    const int maxY = std::max(minTop, h - layout.height);
    int x = fileContextMenu.anchorX;
    int y = fileContextMenu.anchorY;
    if (x < 0 || y < 0) {
      x = (w - layout.width) / 2;
      y = minTop + std::max(0, (std::max(1, h - minTop) - layout.height) / 2);
    }
    x = std::clamp(x, 0, std::max(0, w - layout.width));
    y = std::clamp(y, minTop, maxY);
    layout.x = x;
    layout.y = y;
    layout.listY = y + 1;
    layout.rows = static_cast<int>(items.size());
    layout.valid = true;
    return layout;
  };

  auto runFileContextAction = [&](int actionIndex) {
    if (!fileContextMenu.active) return;
    const FileEntry entry = fileContextMenu.entry;
    if (actionIndex == 0) {
      bool played = false;
      if (entry.trackIndex >= 0) {
        played = tryStartAudioFile(entry.path, entry.trackIndex);
      } else if (callbacks.onPlayFile) {
        played = callbacks.onPlayFile(entry.path);
      }
      (void)played;
    } else if (actionIndex == 1) {
      startMelodyExport(entry);
    } else if (actionIndex == 2) {
      if (!entry.isDir && isSupportedAudioExt(entry.path) &&
          !isBackgroundTaskRunning()) {
        LoopSplitConfig splitConfig;
        splitConfig.channels = 2;
        splitConfig.sampleRate = 48000;
        splitConfig.trackIndex = std::max(0, entry.trackIndex);
        splitConfig.kssOptions = audioGetKssOptionState();
        splitConfig.nsfOptions = audioGetNsfOptionState();
        splitConfig.vgmOptions = audioGetVgmOptionState();
        startLoopSplitExport(entry.path, o.output, splitConfig, loopSplitTask);
      }
    }
    fileContextMenu.active = false;
    dirty = true;
  };

  auto drawMelodyPanel = [&](int top, int panelHeight, int panelWidth,
                             const AudioMelodyInfo& melodyInfo,
                             const AudioMelodyAnalysisState& melodyAnalysisState) {
    if (panelHeight <= 0 || panelWidth <= 0) return;
    int bottom = top + panelHeight;
    int row = top;

    if (row < bottom) {
      screen.writeText(0, row++, fitLine(" Melody", panelWidth), kStyleAccent);
    }

    if (row < bottom) {
      std::string noteLine;
      if (melodyInfo.midiNote >= 0) {
        int hz = static_cast<int>(std::round(melodyInfo.frequencyHz));
        noteLine = " " + midiToNoteName(melodyInfo.midiNote) + "   " +
                   std::to_string(hz) + "Hz";
      } else {
        noteLine = " --";
      }
      screen.writeText(0, row++, fitLine(noteLine, panelWidth), kStyleNormal);
    }

    if (row < bottom) {
      int pct =
          static_cast<int>(std::round(std::clamp(melodyInfo.confidence, 0.0f, 1.0f) *
                                      100.0f));
      int meterWidth = std::max(8, panelWidth - 20);
      int filled =
          static_cast<int>(std::round(static_cast<float>(meterWidth) *
                                      std::clamp(melodyInfo.confidence, 0.0f, 1.0f)));
      filled = std::clamp(filled, 0, meterWidth);
      std::string meter = "[";
      meter.append(static_cast<size_t>(filled), '#');
      meter.append(static_cast<size_t>(std::max(0, meterWidth - filled)), '.');
      meter.push_back(']');
      std::string confLine = " Confidence " + meter + " " + std::to_string(pct) + "%";
      screen.writeText(0, row++, fitLine(confLine, panelWidth), kStyleDim);
    }

    if (row < bottom) {
      std::string statusLine;
      if (!melodyAnalysisState.error.empty()) {
        statusLine = " Analysis: Error - " + melodyAnalysisState.error;
      } else if (melodyAnalysisState.ready) {
        statusLine =
            " Analysis: Ready (" + std::to_string(melodyAnalysisState.frameCount) +
            " pts)";
      } else if (melodyAnalysisState.running) {
        int progressPercent =
            static_cast<int>(std::round(std::clamp(melodyAnalysisState.progress,
                                                   0.0f, 1.0f) *
                                      100.0f));
        statusLine = " Analysis: " + std::to_string(progressPercent) + "%";
      } else {
        statusLine = " Analysis: Idle";
      }
      screen.writeText(0, row++, fitLine(statusLine, panelWidth), kStyleDim);
    }

    int graphTop = row;
    int graphHeight = bottom - graphTop;
    if (graphHeight < 4) return;

    constexpr int kGraphMinMidi = 36;  // C2
    constexpr int kGraphMaxMidi = 96;  // C7
    int labelWidth = (panelWidth >= 34) ? 7 : 0;
    int chartX = labelWidth;
    int chartWidth = panelWidth - chartX;
    if (chartWidth < 8) return;

    auto midiToRow = [&](int midi) {
      int clamped = std::clamp(midi, kGraphMinMidi, kGraphMaxMidi);
      int range = kGraphMaxMidi - kGraphMinMidi;
      if (range <= 0 || graphHeight <= 1) return graphTop;
      float norm =
          static_cast<float>(kGraphMaxMidi - clamped) / static_cast<float>(range);
      int offset = static_cast<int>(std::round(norm * (graphHeight - 1)));
      return graphTop + std::clamp(offset, 0, graphHeight - 1);
    };

    for (int midi = kGraphMinMidi; midi <= kGraphMaxMidi; midi += 12) {
      int y = midiToRow(midi);
      if (y < graphTop || y >= bottom) continue;
      if (labelWidth > 0) {
        std::string label = midiToNoteName(midi);
        if (utf8DisplayWidth(label) < labelWidth) {
          label.insert(label.begin(),
                       static_cast<size_t>(labelWidth - utf8DisplayWidth(label)),
                       ' ');
        }
        screen.writeText(0, y, utf8TakeDisplayWidth(label, labelWidth),
                         kStyleDim);
      }
      screen.writeRun(chartX, y, chartWidth, L'.', kStyleDim);
    }

    size_t sampleCount = melodyHistoryMidi.size();
    if (sampleCount > 0) {
      size_t start = 0;
      if (sampleCount > static_cast<size_t>(chartWidth)) {
        start = sampleCount - static_cast<size_t>(chartWidth);
      }
      for (int x = 0; x < chartWidth; ++x) {
        size_t idx = start + static_cast<size_t>(x);
        if (idx >= sampleCount) break;
        int midi = melodyHistoryMidi[idx];
        if (midi < 0) continue;
        int y = midiToRow(midi);
        float conf = melodyHistoryConfidence[idx];
        Style pointStyle = kStyleDim;
        wchar_t point = L'*';
        if (conf >= 0.75f) {
          pointStyle = kStyleAccent;
          point = L'#';
        } else if (conf >= 0.45f) {
          pointStyle = kStyleNormal;
        }
        screen.writeChar(chartX + x, y, point, pointStyle);
      }
    }

    if (melodyInfo.midiNote >= 0) {
      int y = midiToRow(melodyInfo.midiNote);
      int x = chartX + chartWidth - 1;
      if (x >= chartX && y >= graphTop && y < bottom) {
        screen.writeChar(x, y, L'@', kStyleAccent);
      }
    }
  };

  while (running) {
    if (layoutDirty) {
      rebuildLayout();
    }
    cleanupMelodyExportWorker();
    cleanupLoopSplitExportWorker(loopSplitTask);

    if (windowTuiEnabled && tuiWindow.IsOpen()) {
      tuiWindow.PollEvents();
    }
    if (audioMiniPlayer.isOpen() &&
        audioMiniPlayer.pollEvents(audioMiniCallbacks)) {
      markDirty(UiDirtyFlags::Async);
    }

    if (consumeBrowserThumbnailWake()) {
      markDirty(UiDirtyFlags::Async);
    }

    auto processInputEvent = [&](InputEvent ev) {
      if (ev.type == InputEvent::Type::Resize) {
        dirty = true;
        if (callbacks.onResize) callbacks.onResize();
        return;
      }
      if (dispatchFileDrop(ev, callbacks.onPlayFiles)) {
        markDirty(UiDirtyFlags::Async);
        return;
      }
      if (ev.type == InputEvent::Type::Mouse &&
          isWindowMouseEvent(ev.mouse)) {
        int wndW = std::max(1, tuiWindow.GetWidth());
        int wndH = std::max(1, tuiWindow.GetHeight());
        int gridW = std::max(1, screen.width());
        int gridH = std::max(1, screen.height());
        const int pixelX = ev.mouse.hasPixelPosition ? ev.mouse.pixelX
                                                     : ev.mouse.pos.X;
        const int pixelY = ev.mouse.hasPixelPosition ? ev.mouse.pixelY
                                                     : ev.mouse.pos.Y;
        ev.mouse.hasPixelPosition = true;
        ev.mouse.pixelX = pixelX;
        ev.mouse.pixelY = pixelY;
        ev.mouse.unitWidth = static_cast<double>(wndW) / gridW;
        ev.mouse.unitHeight = static_cast<double>(wndH) / gridH;
        int gx = static_cast<int>((static_cast<int64_t>(pixelX) * gridW) /
                                  wndW);
        int gy = static_cast<int>((static_cast<int64_t>(pixelY) * gridH) /
                                  wndH);
        gx = std::clamp(gx, 0, gridW - 1);
        gy = std::clamp(gy, 0, gridH - 1);
        ev.mouse.pos.X = static_cast<SHORT>(gx);
        ev.mouse.pos.Y = static_cast<SHORT>(gy);
      }
      const bool browserInteractionEnabled = !melodyVisualizationEnabled;
      bool isLeftClick = (ev.type == InputEvent::Type::Mouse) &&
                        (ev.mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
      bool clearBtnHover = false;
      if (ev.type == InputEvent::Type::Mouse) {
        clearBtnHover =
            browserInteractionEnabled && searchBarY >= 0 &&
            ev.mouse.pos.Y == searchBarY && searchBarClearStart >= 0 &&
            searchBarClearEnd > searchBarClearStart &&
            ev.mouse.pos.X >= searchBarClearStart &&
            ev.mouse.pos.X < searchBarClearEnd;
        if (searchBarClearHover != clearBtnHover) {
          searchBarClearHover = clearBtnHover;
          markDirty();
        }
      }
      if (ev.type == InputEvent::Type::Key) {
        const KeyEvent& key = ev.key;
        const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
        bool ctrl = (key.control & ctrlMask) != 0;
        bool paletteToggle =
            ctrl && (key.vk == 'P' || key.ch == 'p' || key.ch == 'P');
        if (paletteToggle) {
          paletteActive = !paletteActive;
          if (paletteActive) {
            fileContextMenu.active = false;
          }
          setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
          paletteQuery.clear();
          paletteSelected = 0;
          paletteScroll = 0;
          markDirty();
          return;
        }
      }
      if (ev.type == InputEvent::Type::Mouse && clearBtnHover && isLeftClick) {
        if (browser.filterActive) {
          browser.filter.clear();
          setBrowserSearchFocus(browser, BrowserSearchFocus::Filter, dirty);
        } else if (browser.pathSearchActive) {
          browser.pathSearch.clear();
          setBrowserSearchFocus(browser, BrowserSearchFocus::PathSearch, dirty);
        } else {
          browser.filterBackup = browser.filter;
          browser.filter.clear();
          setBrowserSearchFocus(browser, BrowserSearchFocus::Filter, dirty);
        }
        if (callbacks.onRefreshBrowser) {
          callbacks.onRefreshBrowser(browser, "");
        }
        markDirty();
        return;
      }
      if (fileContextMenu.active) {
        fileContextLayout =
            computeFileContextLayout(width, height, listTop);
        if (ev.type == InputEvent::Type::Key) {
          const KeyEvent& key = ev.key;
          if (key.vk == VK_ESCAPE) {
            fileContextMenu.active = false;
            dirty = true;
            return;
          }
          if (key.vk == VK_UP) {
            fileContextMenu.selected =
                (fileContextMenu.selected + fileContextLayout.rows - 1) %
                fileContextLayout.rows;
            dirty = true;
            return;
          }
          if (key.vk == VK_DOWN) {
            fileContextMenu.selected =
                (fileContextMenu.selected + 1) % fileContextLayout.rows;
            dirty = true;
            return;
          }
          if (key.vk == VK_RETURN) {
            runFileContextAction(fileContextMenu.selected);
            return;
          }
          return;
        }
        if (ev.type == InputEvent::Type::Mouse) {
          const MouseEvent& mouse = ev.mouse;
          bool leftPressed =
              (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
          if (mouse.eventFlags == MOUSE_WHEELED) {
            int delta = static_cast<SHORT>(HIWORD(mouse.buttonState));
            if (delta != 0) {
              if (delta > 0) {
                fileContextMenu.selected =
                    (fileContextMenu.selected + fileContextLayout.rows - 1) %
                    fileContextLayout.rows;
              } else {
                fileContextMenu.selected =
                    (fileContextMenu.selected + 1) % fileContextLayout.rows;
              }
              dirty = true;
            }
            return;
          }
          if (mouse.eventFlags == 0 && leftPressed) {
            bool inside =
                fileContextLayout.valid &&
                mouse.pos.X >= fileContextLayout.x &&
                mouse.pos.X < fileContextLayout.x + fileContextLayout.width &&
                mouse.pos.Y >= fileContextLayout.y &&
                mouse.pos.Y < fileContextLayout.y + fileContextLayout.height;
            if (inside && mouse.pos.Y >= fileContextLayout.listY &&
                mouse.pos.Y < fileContextLayout.listY + fileContextLayout.rows) {
              int action = mouse.pos.Y - fileContextLayout.listY;
              if (action >= 0 && action < fileContextLayout.rows) {
                fileContextMenu.selected = action;
                runFileContextAction(action);
                return;
              }
            }
            fileContextMenu.active = false;
            dirty = true;
            return;
          }
          return;
        }
      }
      if (paletteActive) {
        auto cmds = buildCommands();
        filterCommands(cmds, &paletteFiltered);
        int maxRows = std::max(1, height - 8);
        int visibleRows =
            std::min(maxRows, std::max(1, static_cast<int>(paletteFiltered.size())));
        ensurePaletteScroll(static_cast<int>(paletteFiltered.size()),
                            visibleRows);
        if (ev.type == InputEvent::Type::Key) {
          const KeyEvent& key = ev.key;
          const DWORD ctrlMask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
          const DWORD altMask = LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED;
          bool ctrl = (key.control & ctrlMask) != 0;
          bool alt = (key.control & altMask) != 0;
          if (key.vk == VK_ESCAPE) {
            paletteActive = false;
            dirty = true;
            return;
          }
          if (key.vk == VK_RETURN) {
            if (!paletteFiltered.empty()) {
              int idx = paletteFiltered[static_cast<size_t>(
                  std::clamp(paletteSelected, 0,
                             static_cast<int>(paletteFiltered.size()) - 1))];
              if (idx >= 0 && idx < static_cast<int>(cmds.size())) {
                cmds[static_cast<size_t>(idx)].run();
              }
            }
            paletteActive = false;
            dirty = true;
            return;
          }
          if (key.vk == VK_UP) {
            paletteSelected--;
            ensurePaletteScroll(static_cast<int>(paletteFiltered.size()),
                                visibleRows);
            dirty = true;
            return;
          }
          if (key.vk == VK_DOWN) {
            paletteSelected++;
            ensurePaletteScroll(static_cast<int>(paletteFiltered.size()),
                                visibleRows);
            dirty = true;
            return;
          }
          if (key.vk == VK_BACK) {
            if (!paletteQuery.empty()) {
              paletteQuery.pop_back();
              paletteSelected = 0;
              paletteScroll = 0;
            }
            dirty = true;
            return;
          }
          if (!ctrl && !alt && key.ch >= 32) {
            paletteQuery.push_back(key.ch);
            paletteSelected = 0;
            paletteScroll = 0;
            dirty = true;
            return;
          }
          return;
        }
        if (ev.type == InputEvent::Type::Mouse) {
          const MouseEvent& mouse = ev.mouse;
          bool leftPressed =
              (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
          if (mouse.eventFlags == MOUSE_WHEELED) {
            int delta = static_cast<SHORT>(HIWORD(mouse.buttonState));
            if (delta != 0) {
              paletteSelected -= delta / WHEEL_DELTA;
              ensurePaletteScroll(static_cast<int>(paletteFiltered.size()),
                                  visibleRows);
              dirty = true;
            }
            return;
          }
          if (leftPressed && mouse.eventFlags == 0) {
            paletteLayout =
                computePaletteLayout(width, height, listTop, visibleRows);
            if (paletteLayout.valid) {
              if (mouse.pos.Y >= paletteLayout.listY &&
                  mouse.pos.Y < paletteLayout.listY + paletteLayout.listRows &&
                  mouse.pos.X >= paletteLayout.x + 1 &&
                  mouse.pos.X < paletteLayout.x + paletteLayout.width - 1) {
                int rel = mouse.pos.Y - paletteLayout.listY;
                int idx = paletteScroll + rel;
                if (idx >= 0 &&
                    idx < static_cast<int>(paletteFiltered.size())) {
                  int cmdIndex =
                      paletteFiltered[static_cast<size_t>(idx)];
                  if (cmdIndex >= 0 &&
                      cmdIndex < static_cast<int>(cmds.size())) {
                    cmds[static_cast<size_t>(cmdIndex)].run();
                  }
                  paletteActive = false;
                  dirty = true;
                  return;
                }
              }
            }
          }
          return;
        }
      }
      handleInputEvent(ev, browser, layout, breadcrumbLine, breadcrumbY,
                       searchBarY, searchBarWidth, listTop, listHeight,
                       progressBarX, progressBarY,
                       progressBarWidth, actionStrip, browserInteractionEnabled,
                       o.play, audioIsReady(), breadcrumbHover, actionHover,
                       searchBarHover,
                       dirty, running, callbacks);
    };

    auto finalizeRenderedExit = [&]() {
      audioMiniPlayer.close();
      if (windowTuiEnabled && tuiWindow.IsOpen()) {
        tuiWindow.Close();
      }
      joinMelodyExportWorker();
      joinLoopSplitExportWorker(loopSplitTask);
      audioShutdown();
    };

    auto flushLayoutIfNeeded = [&]() {
      if (layoutDirty && hasDirtyFlag(dirtyFlags, UiDirtyFlags::Layout)) {
        rebuildLayout();
      }
    };

    auto dispatchInputEvent = [&](const InputEvent& event) -> bool {
      processSystemPlaybackCommands();
      processInputEvent(event);
      if (didRender) {
        finalizeRenderedExit();
        return false;
      }
      return true;
    };

    const BrowserState::ViewMode preInputViewMode = browser.viewMode;
    const bool preInputMelodyVisualization = melodyVisualizationEnabled;
    const bool preInputOptionsMode = optionsBrowserIsActive(browser);
    const bool preInputTrackMode = isTrackBrowserActive(browser);
    InputEvent ev{};
    if (windowTuiEnabled && tuiWindow.IsOpen()) {
      while (running && tuiWindow.PollInput(ev)) {
        if (!dispatchInputEvent(ev)) return 0;
        if (ev.type == InputEvent::Type::Resize) {
          flushLayoutIfNeeded();
        }
        if (!running) break;
      }
    }

    if (running && consoleInputPump.pollNext(input, ev)) {
      if (!dispatchInputEvent(ev)) return 0;
      if (ev.type == InputEvent::Type::Resize) {
        flushLayoutIfNeeded();
      }
    }
    if (didRender) {
      finalizeRenderedExit();
      return 0;
    }
    if (!running) break;

    processSystemPlaybackCommands();
    syncAudioSystemControls();

    BrowserFooterLayout nextFooterLayout = buildFooterLayout();
    if (nextFooterLayout != footerLayout) {
      footerLayout = nextFooterLayout;
      markLayoutDirty();
    }

    if (browser.viewMode != preInputViewMode ||
        melodyVisualizationEnabled != preInputMelodyVisualization ||
        optionsBrowserIsActive(browser) != preInputOptionsMode ||
        isTrackBrowserActive(browser) != preInputTrackMode) {
      markLayoutDirty();
    }

    auto now = std::chrono::steady_clock::now();
    auto computeWakeTimeout = [&](std::chrono::steady_clock::time_point nowTime) {
      DWORD timeout = INFINITE;
      auto reduceTimeout = [&](std::chrono::milliseconds interval) {
        if (interval.count() <= 0) {
          timeout = 0;
          return;
        }
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(nowTime -
                                                                  lastDraw);
        long long remaining = interval.count() - elapsed.count();
        DWORD candidate =
            remaining <= 0 ? 0u : static_cast<DWORD>(remaining);
        timeout = (timeout == INFINITE) ? candidate : std::min(timeout, candidate);
      };

      if (viewport.browserInteractionEnabled &&
          (browser.filterActive || browser.pathSearchActive)) {
        reduceTimeout(std::chrono::milliseconds(250));
      }
      if (melodyVisualizationEnabled) {
        reduceTimeout(std::chrono::milliseconds(50));
      }
      if (o.play &&
          (audioIsReady() || !audioGetNowPlaying().empty() || seekHoldActive)) {
        reduceTimeout(std::chrono::milliseconds(100));
      }
      if (audioMiniPlayer.isOpen()) {
        reduceTimeout(std::chrono::milliseconds(100));
      }
      if (isBackgroundTaskRunning()) {
        reduceTimeout(std::chrono::milliseconds(100));
      }
      return timeout;
    };

    if (!dirty) {
      DWORD waitTimeout = computeWakeTimeout(now);
      DWORD waitResult =
          waitForBrowserWake(input, browserThumbnailWakeHandle(), waitTimeout);
      if (consumeBrowserThumbnailWake()) {
        markDirty(UiDirtyFlags::Async);
      } else if (waitResult == WAIT_TIMEOUT) {
        markDirty();
      }
      continue;
    }
    if (layoutDirty && hasDirtyFlag(dirtyFlags, UiDirtyFlags::Layout)) {
      rebuildLayout();
    }
    if (dirty) {
      bool optionsMode = optionsBrowserIsActive(browser);
      bool trackMode = isTrackBrowserActive(browser);
      bool browserInteractionEnabled = !melodyVisualizationEnabled;

      screen.clear(kStyleNormal);
      screen.setAlwaysFullRedraw(forceFullRedraw);

      const std::string headerTitleRaw = "Radioify";
      std::string headerTitle = headerTitleRaw;
      if (static_cast<int>(headerTitle.size()) > width) {
        headerTitle = fitLine(headerTitleRaw, width);
      }
      int headerTitleLen = static_cast<int>(headerTitle.size());
      int headerTitleX = std::max(0, (width - headerTitleLen) / 2);
      double seconds =
          std::chrono::duration<double>(now.time_since_epoch()).count();
      double speed = 1.3;
      float pulse = static_cast<float>(0.5 * (std::sin(seconds * speed) + 1.0));
      pulse = clamp01(pulse);
      pulse = pulse * pulse * (3.0f - 2.0f * pulse);
      float t = std::pow(pulse, 0.6f);
      float flash = 0.0f;
      if (t > 0.88f) {
        flash = (t - 0.88f) / 0.12f;
        flash = flash * flash;
      }
      Color headerBg = lerpColor(kStyleHeader.bg, kStyleHeaderHot.bg,
                                 std::min(0.85f, t * 0.9f));
      headerBg = lerpColor(headerBg, Color{52, 44, 26}, flash * 0.7f);
      Style headerLineStyle{kStyleHeader.fg, headerBg};
      screen.writeRun(0, 0, width, L' ', headerLineStyle);

      Color titleFg;
      if (t < 0.35f) {
        titleFg = lerpColor(kStyleHeader.fg, kStyleHeaderGlow.fg, t / 0.35f);
      } else {
        float hotT = (t - 0.35f) / 0.65f;
        titleFg =
            lerpColor(kStyleHeaderGlow.fg, kStyleHeaderHot.fg, clamp01(hotT));
      }
      if (t > 0.85f) {
        float whiteT = (t - 0.85f) / 0.15f;
        titleFg = lerpColor(titleFg, Color{255, 255, 255}, clamp01(whiteT));
      }
      if (flash > 0.0f) {
        titleFg = lerpColor(titleFg, Color{255, 236, 186}, clamp01(flash));
      }
      Style titleAttr{titleFg, headerBg};
      for (int i = 0; i < headerTitleLen; ++i) {
        wchar_t ch = static_cast<wchar_t>(
            static_cast<unsigned char>(headerTitle[static_cast<size_t>(i)]));
        screen.writeChar(headerTitleX + i, 0, ch, titleAttr);
      }
      breadcrumbLine = buildBreadcrumbLine(browser.dir, width);
      if (browserInteractionEnabled) {
        const bool browserSearchFocused =
            browser.filterActive || browser.pathSearchActive;
        Style searchStyle =
            browserSearchFocused
                ? kStyleSearchBarActive
                : (searchBarHover ? kStyleSearchBarGlow : kStyleSearchBar);
        screen.writeRun(0, searchBarY, width, L' ', searchStyle);
        bool showSearchCursor =
            browserSearchFocused &&
            ((std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch())
                  .count() /
              500) %
             2) == 0;
        const bool usingPathSearch = browser.pathSearchActive;
        const std::string searchText =
            usingPathSearch ? browser.pathSearch
                            : (browser.filter.empty() ? "type to filter"
                                                     : browser.filter);
        const std::string searchLine = std::string(usingPathSearch ? " Path: " : " Search: ") + searchText;
        int searchTextWidth = std::max(
            1, width - searchBarClearButtonWidth);
        std::string shownSearchLine = fitLine(searchLine, searchTextWidth);
        screen.writeText(0, searchBarY, shownSearchLine, searchStyle);
        if (showSearchCursor && searchTextWidth > 0) {
          int cursorX =
              std::min(searchTextWidth - 1, utf8DisplayWidth(shownSearchLine));
          screen.writeChar(cursorX, searchBarY, L'\u2588', searchStyle);
        }
        if (searchBarClearStart >= 0 && searchBarClearEnd > searchBarClearStart) {
          Style clearStyle =
              searchBarClearHover ? kStyleSearchBarActive : searchStyle;
          screen.writeText(searchBarClearStart, searchBarY,
                           fitLine(" [x] ", searchBarClearEnd - searchBarClearStart),
                           clearStyle);
        }

        if (breadcrumbHover >= static_cast<int>(breadcrumbLine.crumbs.size())) {
          breadcrumbHover = -1;
        }
        screen.writeText(0, breadcrumbY, breadcrumbLine.text, kStyleAccent);
        if (breadcrumbHover >= 0) {
          const auto& crumb =
              breadcrumbLine.crumbs[static_cast<size_t>(breadcrumbHover)];
          std::string hoverText = utf8SliceDisplayWidth(
              breadcrumbLine.text, crumb.startX, crumb.endX - crumb.startX);
          screen.writeText(crumb.startX, breadcrumbY, hoverText,
                           kStyleBreadcrumbHover);
        }
      } else {
        breadcrumbHover = -1;
      }
      std::filesystem::path nowPlaying = audioGetNowPlaying();
      int nowPlayingTrackIndex = audioGetTrackIndex();
      if (nowPlaying != lastMelodyTrack ||
          nowPlayingTrackIndex != lastMelodyTrackIndex) {
        clearMelodyHistory();
        lastMelodyTrack = nowPlaying;
        lastMelodyTrackIndex = nowPlayingTrackIndex;
      }
      std::string showingLabel;
      if (!browserInteractionEnabled) {
        showingLabel.clear();
      } else if (optionsMode) {
        showingLabel = optionsBrowserShowingLabel();
      } else if (trackMode) {
        showingLabel =
            "  Showing: tracks in " + toUtf8String(browser.dir.filename());
      } else {
        showingLabel.clear();
      }
      if (!showingLabel.empty()) {
        screen.writeText(0, std::min(height - 1, searchBarY + 1),
                         fitLine(showingLabel, width), kStyleDim);
      }
      AudioMelodyInfo melodyInfo = audioGetMelodyInfo();
      AudioMelodyAnalysisState melodyAnalysisState = audioGetMelodyAnalysisState();
      if (melodyAnalysisState.running && !lastMelodyAnalysisRunning) {
        clearMelodyHistory();
      }
      lastMelodyAnalysisRunning = melodyAnalysisState.running;
      if (melodyVisualizationEnabled && !audioIsPaused() && !audioIsHolding()) {
        pushMelodyHistory(melodyInfo);
      }
      if (browserInteractionEnabled) {
        drawBrowserEntries(screen, browser, layout, listTop, listHeight,
                           kStyleNormal, kStyleNormal, kStyleDir,
                           kStyleHighlight, kStyleDim, isSupportedImageExt,
                           isVideoExt, isSupportedAudioExt);
      } else {
        drawMelodyPanel(listTop, listHeight, width, melodyInfo,
                        melodyAnalysisState);
      }

      int footerStart = listTop + listHeight;
      int line = footerStart;
      if (line < height && footerLayout.showMeta) {
        std::string meta;
        if (optionsMode) {
          meta = optionsBrowserSelectionMeta(browser);
        } else if (trackMode) {
          meta = buildTrackSelectionMeta(browser);
        } else {
          meta = buildSelectionMeta(browser, isVideoExt);
        }
        if (!meta.empty()) {
          screen.writeText(0, line++, fitLine(meta, width), kStyleDim);
        }
      }
      if (line < height && footerLayout.showWarning) {
        std::string warning = audioGetWarning();
        if (!warning.empty()) {
          screen.writeText(0, line++, fitLine("  Warning: " + warning, width),
                           kStyleDim);
        }
      }
      if (line < height) {
        bool exportHasResult = false;
        bool exportSuccess = false;
        std::string exportStatus;
        {
          std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
          exportHasResult = melodyExportTask.hasResult;
          exportSuccess = melodyExportTask.success;
          exportStatus = melodyExportTask.status;
        }
        if (footerLayout.showAnalyzeStatus && exportHasResult &&
            !exportStatus.empty()) {
          Style statusStyle = exportSuccess ? kStyleDim : kStyleAlert;
          screen.writeText(0, line++,
                           fitLine(" Analyze: " + exportStatus, width),
                           statusStyle);
        }
        bool loopSplitHasResult = false;
        bool loopSplitSuccess = false;
        std::string loopSplitStatus;
        {
          std::lock_guard<std::mutex> lock(loopSplitTask.mutex);
          loopSplitHasResult = loopSplitTask.hasResult;
          loopSplitSuccess = loopSplitTask.success;
          loopSplitStatus = loopSplitTask.status;
        }
        if (footerLayout.showLoopSplitStatus && loopSplitHasResult &&
            !loopSplitStatus.empty()) {
          Style statusStyle = loopSplitSuccess ? kStyleDim : kStyleAlert;
          screen.writeText(0, line++,
                           fitLine(" Loop Split: " + loopSplitStatus, width),
                           statusStyle);
        }
      }
      std::string nowLabel = buildAudioNowPlayingLabel();
      if (footerLayout.showNowPlaying) {
        const int nowStart = line;
        const int nowPlayingLines = std::max(1, footerLayout.nowPlayingLines);
        std::vector<std::string> lines =
            wrapLine(std::string(" ") + nowLabel, width);
        for (int i = 0; i < nowPlayingLines &&
                        i < static_cast<int>(lines.size());
             ++i) {
          const int y = nowStart + i;
          if (y >= height) break;
          screen.writeText(0, y, lines[static_cast<size_t>(i)],
                           kStyleAccent);
        }
        line = nowStart + nowPlayingLines;
      }

      actionStrip.buttons.clear();
      actionStrip.y = -1;
      if (footerLayout.showActionStrip && line < height) {
        actionStrip.y = line;
        std::vector<ActionRenderItem> items =
            buildActionRenderItems(browserInteractionEnabled);
        const int gapWidth = 2;
        int x = 0;
        int itemLine = line;
        for (const auto& item : items) {
          int widthUsed = std::min(std::max(1, item.width), width);
          const int gap = x > 0 ? gapWidth : 0;
          if (x > 0 && x + gap + widthUsed > width) {
            ++itemLine;
            x = 0;
          } else {
            x += gap;
          }
          if (itemLine >= height) break;
          bool hovered =
              (actionHover == static_cast<int>(actionStrip.buttons.size()));
          std::string text = hovered ? item.labelHover : item.label;
          int textWidth = utf8DisplayWidth(text);
          widthUsed = std::min(widthUsed, width - x);
          if (widthUsed <= 0) break;
          if (textWidth > widthUsed) {
            text = utf8TakeDisplayWidth(text, widthUsed);
            textWidth = utf8DisplayWidth(text);
          }
          if (textWidth <= 0) break;
          if (textWidth < widthUsed) {
            text.append(static_cast<size_t>(widthUsed - textWidth), ' ');
            textWidth = widthUsed;
          }
          ActionStripButton btn{item.id, x, x + widthUsed, itemLine};
          actionStrip.buttons.push_back(btn);
          Style style = item.active ? kStyleActionActive : kStyleNormal;
          screen.writeText(x, itemLine, text, style);
          x += widthUsed;
        }
        if (actionHover >= static_cast<int>(actionStrip.buttons.size())) {
          actionHover = -1;
        }
        line += std::max(1, footerLayout.actionStripLines);
      } else {
        actionHover = -1;
      }

      int peakMeterY = -1;
      if (footerLayout.showPeakMeter && line < height) {
        peakMeterY = line;
        line++;
      }

      bool audioReady = audioIsReady();
      double currentSec = audioReady ? audioGetTimeSec() : 0.0;
      double totalSec = audioReady ? audioGetTotalSec() : -1.0;
      double displaySec = currentSec;
      if (audioReady) {
        if (audioIsSeeking()) {
          double seekSec = audioGetSeekTargetSec();
          if (seekSec >= 0.0) {
            displaySec = seekSec;
            seekDisplaySec = seekSec;
            seekHoldActive = true;
            seekHoldStart = now;
          }
        } else if (seekHoldActive) {
          double diff = std::abs(currentSec - seekDisplaySec);
          auto holdAge = now - seekHoldStart;
          if ((audioIsPrimed() && diff <= 0.25) ||
              holdAge > std::chrono::seconds(2)) {
            seekHoldActive = false;
          } else {
            displaySec = seekDisplaySec;
          }
        }
      } else {
        seekHoldActive = false;
      }
      int volPct = static_cast<int>(std::round(audioGetVolume() * 100.0f));
      double ratio = 0.0;
      if (totalSec > 0.0 && std::isfinite(totalSec)) {
        ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
      }
      ProgressFooterStyles footerStyles{kStyleNormal,
                                        kStyleProgressEmpty,
                                        kStyleProgressFrame,
                                        kStyleAlert,
                                        kStyleAccent,
                                        kProgressStart,
                                        kProgressEnd};
      ProgressFooterInput footerInput;
      footerInput.displaySec = displaySec;
      footerInput.totalSec = totalSec;
      footerInput.ratio = ratio;
      footerInput.volPct = volPct;
      footerInput.width = width;
      footerInput.progressY = line;
      footerInput.peakY = peakMeterY;
      footerInput.peak = audioGetPeak();
      footerInput.clipAlert = audioHasClipAlert();
      ProgressFooterRenderResult footerResult =
          renderProgressFooter(screen, footerInput, footerStyles);
      progressBarX = footerResult.progressBarX;
      progressBarY = footerResult.progressBarY;
      progressBarWidth = footerResult.progressBarWidth;

      if (paletteActive) {
        auto cmds = buildCommands();
        filterCommands(cmds, &paletteFiltered);
        int maxRows = std::max(1, height - listTop - 3);
        int visibleRows =
            std::min(maxRows, std::max(1, static_cast<int>(paletteFiltered.size())));
        ensurePaletteScroll(static_cast<int>(paletteFiltered.size()),
                            visibleRows);
        paletteLayout =
            computePaletteLayout(width, height, listTop, visibleRows);
        if (paletteLayout.valid) {
          int x0 = paletteLayout.x;
          int y0 = paletteLayout.y;
          int w = paletteLayout.width;
          int h = paletteLayout.height;
          int inner = paletteLayout.innerWidth;

          // Fill background
          for (int y = 0; y < h; ++y) {
            screen.writeRun(x0, y0 + y, w, L' ', kStyleNormal);
          }

          // Border
          screen.writeChar(x0, y0, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0, w - 2, L'-', kStyleDim);
          screen.writeChar(x0 + w - 1, y0, L'+', kStyleDim);
          screen.writeChar(x0, y0 + h - 1, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0 + h - 1, w - 2, L'-', kStyleDim);
          screen.writeChar(x0 + w - 1, y0 + h - 1, L'+', kStyleDim);
          for (int y = 1; y < h - 1; ++y) {
            screen.writeChar(x0, y0 + y, L'|', kStyleDim);
            screen.writeChar(x0 + w - 1, y0 + y, L'|', kStyleDim);
          }

          std::string prompt = "> " + paletteQuery;
          if (utf8DisplayWidth(prompt) > inner) {
            prompt = utf8TakeDisplayWidth(prompt, inner);
          }
          screen.writeText(x0 + 1, paletteLayout.inputY, prompt, kStyleNormal);

          if (paletteFiltered.empty()) {
            std::string none = "(no matches)";
            if (utf8DisplayWidth(none) > inner) {
              none = utf8TakeDisplayWidth(none, inner);
            }
            screen.writeText(x0 + 1, paletteLayout.listY, none, kStyleDim);
          } else {
            for (int row = 0; row < paletteLayout.listRows; ++row) {
              int idx = paletteScroll + row;
              if (idx < 0 ||
                  idx >= static_cast<int>(paletteFiltered.size())) {
                break;
              }
              const auto& cmd =
                  cmds[static_cast<size_t>(paletteFiltered[static_cast<size_t>(idx)])];
              std::string left = cmd.label;
              std::string right = cmd.hotkey;
              int rightWidth = utf8DisplayWidth(right);
              int gap = rightWidth > 0 ? 1 : 0;
              int maxLeft = std::max(0, inner - rightWidth - gap);
              if (utf8DisplayWidth(left) > maxLeft) {
                left = utf8TakeDisplayWidth(left, maxLeft);
              }
              int leftWidth = utf8DisplayWidth(left);
              std::string lineText = left;
              if (leftWidth < maxLeft) {
                lineText.append(static_cast<size_t>(maxLeft - leftWidth), ' ');
              }
              if (rightWidth > 0) {
                lineText.push_back(' ');
                lineText += right;
              }
              Style lineStyle =
                  (idx == paletteSelected) ? kStyleHighlight : kStyleNormal;
              screen.writeText(x0 + 1, paletteLayout.listY + row, lineText,
                               lineStyle);
            }
          }
        }
      } else {
        paletteLayout.valid = false;
      }

      if (fileContextMenu.active) {
        fileContextLayout = computeFileContextLayout(width, height, listTop);
        if (fileContextLayout.valid) {
          int x0 = fileContextLayout.x;
          int y0 = fileContextLayout.y;
          int w = fileContextLayout.width;
          int h = fileContextLayout.height;

          for (int y = 0; y < h; ++y) {
            screen.writeRun(x0, y0 + y, w, L' ', kStyleNormal);
          }

          screen.writeChar(x0, y0, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0, w - 2, L'-', kStyleDim);
          screen.writeChar(x0 + w - 1, y0, L'+', kStyleDim);
          screen.writeChar(x0, y0 + h - 1, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0 + h - 1, w - 2, L'-', kStyleDim);
          screen.writeChar(x0 + w - 1, y0 + h - 1, L'+', kStyleDim);
          for (int y = 1; y < h - 1; ++y) {
            screen.writeChar(x0, y0 + y, L'|', kStyleDim);
            screen.writeChar(x0 + w - 1, y0 + y, L'|', kStyleDim);
          }

          std::vector<std::string> items = {" Play", " Analyze", " Split Loop"};
          int inner = std::max(1, w - 2);
          for (int i = 0; i < fileContextLayout.rows; ++i) {
            if (i < 0 || i >= static_cast<int>(items.size())) {
              continue;
            }
            std::string text = items[static_cast<size_t>(i)];
            if (utf8DisplayWidth(text) > inner) {
              text = utf8TakeDisplayWidth(text, inner);
            }
            int textWidth = utf8DisplayWidth(text);
            if (textWidth < inner) {
              text.append(static_cast<size_t>(inner - textWidth), ' ');
            }
            Style rowStyle =
                (i == fileContextMenu.selected) ? kStyleHighlight : kStyleNormal;
            screen.writeText(x0 + 1, fileContextLayout.listY + i, text, rowStyle);
          }
        }
      } else {
        fileContextLayout.valid = false;
      }

      {
        bool isRunning = false;
        float exportProgress = 0.0f;
        std::filesystem::path exportSource;
        std::string title = " Melody Analysis";
        {
          std::lock_guard<std::mutex> lock(melodyExportTask.mutex);
          if (melodyExportTask.running) {
            isRunning = true;
            exportProgress = std::clamp(melodyExportTask.progress, 0.0f, 1.0f);
            exportSource = melodyExportTask.sourceFile;
            title = " Melody Analysis";
          }
        }
        if (!isRunning) {
          std::lock_guard<std::mutex> lock(loopSplitTask.mutex);
          if (loopSplitTask.running) {
            isRunning = true;
            exportProgress = std::clamp(loopSplitTask.progress, 0.0f, 1.0f);
            exportSource = loopSplitTask.sourceFile;
            title = " Loop Split";
          }
        }
        if (isRunning) {
          std::string sourceName =
              exportSource.empty() ? std::string("(unknown)")
                                   : toUtf8String(exportSource.filename());
          int popupWidth = std::max(46, utf8DisplayWidth(sourceName) + 8);
          popupWidth = std::min(width - 2, popupWidth);
          popupWidth = std::max(24, popupWidth);
          int availableHeight = std::max(1, height - listTop);
          int popupHeight = std::clamp(availableHeight, 5, 7);
          int x0 = std::max(0, (width - popupWidth) / 2);
          int y0 =
              listTop + std::max(0, (availableHeight - popupHeight) / 2);

          for (int y = 0; y < popupHeight; ++y) {
            screen.writeRun(x0, y0 + y, popupWidth, L' ', kStyleNormal);
          }
          screen.writeChar(x0, y0, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0, popupWidth - 2, L'-', kStyleDim);
          screen.writeChar(x0 + popupWidth - 1, y0, L'+', kStyleDim);
          screen.writeChar(x0, y0 + popupHeight - 1, L'+', kStyleDim);
          screen.writeRun(x0 + 1, y0 + popupHeight - 1, popupWidth - 2, L'-',
                          kStyleDim);
          screen.writeChar(x0 + popupWidth - 1, y0 + popupHeight - 1, L'+',
                           kStyleDim);
          for (int y = 1; y < popupHeight - 1; ++y) {
            screen.writeChar(x0, y0 + y, L'|', kStyleDim);
            screen.writeChar(x0 + popupWidth - 1, y0 + y, L'|', kStyleDim);
          }

          screen.writeText(x0 + 1, y0 + 1, fitLine(title, popupWidth - 2),
                           kStyleAccent);
          std::string fileLine = " " + sourceName;
          screen.writeText(x0 + 1, y0 + 2, fitLine(fileLine, popupWidth - 2),
                           kStyleDim);

          int barInner = std::max(8, popupWidth - 8);
          int filled = static_cast<int>(
              std::round(static_cast<float>(barInner) * exportProgress));
          filled = std::clamp(filled, 0, barInner);
          std::string bar = "[";
          bar.append(static_cast<size_t>(filled), '#');
          bar.append(static_cast<size_t>(std::max(0, barInner - filled)), '.');
          bar.push_back(']');
          screen.writeText(x0 + 2, y0 + 4, fitLine(bar, popupWidth - 4),
                           kStyleNormal);

          int pct = static_cast<int>(std::round(exportProgress * 100.0f));
          std::string pctLine = " " + std::to_string(pct) + "%";
          screen.writeText(x0 + 2, y0 + 5, fitLine(pctLine, popupWidth - 4),
                           kStyleDim);
        }
      }

      screen.draw();
      screen.setAlwaysFullRedraw(false);
      if (windowTuiEnabled && tuiWindow.IsOpen()) {
        int gridW = 0;
        int gridH = 0;
        if (screen.snapshot(windowCells, gridW, gridH)) {
          tuiWindow.PresentTextGrid(windowCells, gridW, gridH, true);
        }
      }
      renderAudioMiniPlayer();
      lastDraw = now;
      dirty = false;
      dirtyFlags = UiDirtyFlags::None;
      forceFullRedraw = false;
    }
  }
  // Clear screen before exiting
  screen.clear(kStyleNormal);
  screen.draw();
  audioMiniPlayer.close();
  if (windowTuiEnabled && tuiWindow.IsOpen()) {
    tuiWindow.Close();
  }
  input.restore();
  screen.restore();
  std::cout << "\n";
  joinMelodyExportWorker();
  joinLoopSplitExportWorker(loopSplitTask);
  audioShutdown();
  return 0;
}
