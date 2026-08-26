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
#include <functional>
#include <iostream>
#include <mutex>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "app_common.h"
#include "app/playback_queue.h"
#include "app/playback_route.h"
#include "audio_picture_in_picture_window.h"
#include "audioplayback.h"
#include "browser_playback_reveal.h"
#include "browser_playback_source.h"
#include "browser_navigation.h"
#include "browser_model.h"
#include "browsermeta.h"
#include "consoleinput.h"
#include "consolescreen.h"
#include "media_artwork_sidecar.h"
#include "core/open_file_requests.h"
#include "core/latest_request_worker.h"
#include "core/windows_app_resources.h"
#include "core/windows_message_pump.h"
#include "core/windows_console_window.h"
#include "core/windows_shell_open.h"
#include "image_viewer.h"
#include "image_viewer_sequence.h"
#include "media_processing_coordinator.h"
#include "app/media_processing_playback_service.h"
#include "m4adecoder.h"
#include "miniaudio.h"
#include "optionsbrowser.h"
#include "core/path_identity.h"
#include "playback_dialog.h"
#include "calibration_report.h"
#include "radio.h"
#include "audiofilter/radio1938/radio_buffer_io.h"
#include "audiofilter/radio1938/preview/radio_preview_pipeline.h"
#include "playback/control/command.h"
#include "playback/control/transport.h"
#include "playback/input/shortcuts.h"
#include "playback/media_action_catalog.h"
#include "playback/media_processing_actions.h"
#include "playback/media/track_catalog.h"
#include "playback/notification_area/controls.h"
#include "playback/overlay/overlay.h"
#include "playback/session/session.h"
#include "playback/system_media_transport/controls.h"
#include "playback_target_match.h"
#include "playback/target.h"
#include "mouse_double_click_tracker.h"
#include "tracklist.h"
#include "track_browser_state.h"
#include "loopsplit_cli.h"
#include "tui_export.h"
#include "ui_footer_layout.h"
#include "ui_helpers.h"
#include "ui_inputlogic.h"
#include "ui_input_pump.h"
#include "ui_viewport.h"
#include "media_task_card.h"
#include "media_task_presentation.h"
#include "playback/video/playback.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/framebuffer/window/window.h"
#include "windows_file_drop_apartment.h"
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

static DWORD waitForBrowserWake(ConsoleInput& input,
                                const OpenFileRequests& openFileRequests,
                                NativeWaitHandle thumbnailWakeHandle,
                                NativeWaitHandle browserContentWakeHandle,
                                NativeWaitHandle browserMetadataWakeHandle,
                                NativeWaitHandle notificationAreaHandle,
                                const VideoWindow& browserWindow,
                                const AudioPictureInPictureWindow&
                                    audioPictureInPicture,
                                const std::vector<NativeWaitHandle>&
                                    playbackActivityHandles,
                                DWORD timeoutMs) {
  std::vector<NativeWaitHandle> handles;
  handles.reserve(10 + playbackActivityHandles.size());
  const auto append = [&](NativeWaitHandle handle) {
    if (handle) handles.push_back(handle);
  };
  append(input.waitHandle());
  append(openFileRequests.nativeWaitHandle());
  append(thumbnailWakeHandle);
  append(browserContentWakeHandle);
  append(browserMetadataWakeHandle);
  append(notificationAreaHandle);
  if (browserWindow.IsOpen()) {
    append(browserWindow.InputWaitHandle());
    append(browserWindow.CloseRequestedWaitHandle());
  }
  if (audioPictureInPicture.isOpen()) {
    append(audioPictureInPicture.inputWaitHandle());
    append(audioPictureInPicture.closeRequestedWaitHandle());
  }
  for (NativeWaitHandle handle : playbackActivityHandles) append(handle);
  return waitForHandlesAndPumpThreadWindowMessages(
      static_cast<DWORD>(handles.size()),
      handles.empty() ? nullptr : handles.data(), timeoutMs);
}

struct WindowClientSize {
  int width = 1;
  int height = 1;
};

static int gridPixelExtent(int cells, double pixelsPerCell,
                           int fallbackPixelsPerCell) {
  const int resolvedCells = std::max(1, cells);
  const double resolvedPixelsPerCell =
      pixelsPerCell > 0.0 ? pixelsPerCell
                          : static_cast<double>(fallbackPixelsPerCell);
  return std::max(
      1, static_cast<int>(std::lround(resolvedCells * resolvedPixelsPerCell)));
}

static WindowClientSize initialWindowTuiClientSize(const ConsoleScreen& screen) {
  constexpr int kFallbackCellPixelWidth = 10;
  constexpr int kFallbackCellPixelHeight = 20;
  return {
      gridPixelExtent(screen.width(), screen.cellPixelWidth(),
                      kFallbackCellPixelWidth),
      gridPixelExtent(screen.height(), screen.cellPixelHeight(),
                      kFallbackCellPixelHeight),
  };
}

static ShellOpenMode resolveWindowsShellOpenMode(const Options& options) {
  return resolveShellOpenModeSelection(options.shellOpenMode,
                                       configuredWindowsShellOpenMode());
}

static bool shouldForwardInputToExistingInstance(const Options& options,
                                                 ShellOpenMode mode) {
  if (mode != ShellOpenMode::SameInstance || options.input.empty()) {
    return false;
  }
  if (options.extractSheet || options.splitLoop || options.renderRadio) {
    return false;
  }
  return true;
}

static bool isVideoExt(const std::filesystem::path& p) {
  return isSupportedVideoExt(p);
}

static std::optional<PlaybackPresentationState> routeVideoPresentation(
    OpenPresentationDirective directive, bool launchAsciiEnabled) {
  switch (directive) {
    case OpenPresentationDirective::TerminalAscii:
      return PlaybackPresentationState::terminalAscii();
    case OpenPresentationDirective::NativeWindowedFramebuffer:
      return PlaybackPresentationState::nativeWindowed();
    case OpenPresentationDirective::InheritActive:
      return std::nullopt;
    case OpenPresentationDirective::UseLaunchDefaults:
      return launchAsciiEnabled
                 ? PlaybackPresentationState::terminalAscii()
                 : PlaybackPresentationState::nativeWindowed();
  }
  return std::nullopt;
}

static std::optional<playback_route::Route> resolveOpenFilesPlaybackRoute(
    const OpenFilesRequest& request, bool launchAsciiEnabled) {
  std::optional<playback_route::Route> route =
      playback_route::resolveDroppedTarget(
          request.files, nullptr,
          routeVideoPresentation(request.presentation, launchAsciiEnabled));
  return route;
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

using BrowserContentWorker =
    LatestRequestWorker<BrowserContentRequest, BrowserPreparationResult>;

struct BrowserPreparationCancelled {};

using DirectoryListingResult =
    std::variant<std::vector<BrowserEntry>, BrowserPreparationCancelled,
                 BrowserPreparationError>;
using BrowserPopulationResult =
    std::variant<std::monostate, BrowserPreparationCancelled,
                 BrowserPreparationError>;

static BrowserPreparationError browserPreparationError(
    BrowserPreparationErrorKind kind, const BrowserLocation& location,
    std::string message) {
  return {kind, location, std::move(message)};
}

static DirectoryListingResult listEntries(
    const BrowserLocation& location,
    const BrowserContentWorker::Cancellation* cancellation) {
  const std::filesystem::path& dir = location.path();
  std::vector<BrowserEntry> entries;
  std::vector<BrowserEntry> items;
  std::vector<BrowserEntry> knownFolders;
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
    if (cancellation && cancellation->requested()) return;
    if (path.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(path, ec) || ec) {
      return;
    }
    knownFolders.emplace_back(name, path, browser_entry::OpenDirectory{});
  };

  auto appendSectionHeader = [&](const std::string& name) {
    entries.emplace_back(name, std::filesystem::path{},
                         browser_entry::SectionHeader{});
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
        entries.emplace_back(drive.label, drive.path,
                             browser_entry::OpenDirectory{});
      }
    }
    addWindowsKnownFolders();
    if (!knownFolders.empty()) {
      appendSectionHeader("Locations");
      entries.insert(entries.end(), knownFolders.begin(), knownFolders.end());
    }
    if (cancellation && cancellation->requested()) {
      return BrowserPreparationCancelled{};
    }
    return entries;
  } else if (browseDir == browseDir.root_path()) {
    entries.emplace_back("..", std::filesystem::path{},
                         browser_entry::NavigateUp{});
  }
#endif

  if (browseDir.has_parent_path() && browseDir != browseDir.root_path()) {
    entries.emplace_back("..", browseDir.parent_path(),
                         browser_entry::NavigateUp{});
  }

  std::error_code iteratorError;
  std::filesystem::directory_iterator iterator(
      browseDir, std::filesystem::directory_options::none,
      iteratorError);
  if (iteratorError) {
    return browserPreparationError(
        BrowserPreparationErrorKind::Unavailable, location,
        "Unable to open this folder: " + iteratorError.message());
  }
  const std::filesystem::directory_iterator end;
  while (iterator != end) {
    if (cancellation && cancellation->requested()) {
      return BrowserPreparationCancelled{};
    }
    const auto& entry = *iterator;
    const auto& p = entry.path();
    std::error_code ec;
    if (entry.is_directory(ec) && !ec) {
      BrowserEntry item{toUtf8String(p.filename()), p,
                        browser_entry::OpenDirectory{}};
      item.sortMetadata.modifiedAt = entry.last_write_time(ec);
      if (ec) {
        item.sortMetadata.modifiedAt.reset();
      }
      items.push_back(std::move(item));
    } else if (entry.is_regular_file(ec) && !ec && isSupportedMediaExt(p) &&
               !shouldHideBrowserMediaMetadataFile(entry)) {
      BrowserEntry item{toUtf8String(p.filename()), p,
                        browser_entry::OpenFile{}};
      item.sortMetadata.modifiedAt = entry.last_write_time(ec);
      if (ec) {
        item.sortMetadata.modifiedAt.reset();
      }
      item.sortMetadata.size = entry.file_size(ec);
      if (ec) {
        item.sortMetadata.size.reset();
      }
      items.push_back(std::move(item));
    }
    iterator.increment(iteratorError);
    if (iteratorError) {
      return browserPreparationError(
          BrowserPreparationErrorKind::Unavailable, location,
          "Unable to enumerate this folder: " + iteratorError.message());
    }
  }

  if (cancellation && cancellation->requested()) {
    return BrowserPreparationCancelled{};
  }

  entries.insert(entries.end(), items.begin(), items.end());
  return entries;
}

static BrowserPopulationResult populateBrowser(
    BrowserState& state, const std::string& initialName,
    const OptionsBrowserRuntimeSnapshot& optionsRuntime,
    const BrowserContentWorker::Cancellation* cancellation) {
  if (cancellation && cancellation->requested()) {
    return BrowserPreparationCancelled{};
  }
  const bool optionsActive =
      state.location.kind() == BrowserLocationKind::OptionsBrowser;
  if (optionsActive) {
    if (!optionsBrowserSupportsLocation(state.location)) {
      return browserPreparationError(
          BrowserPreparationErrorKind::Unsupported, state.location,
          "Options are not available for this browser location.");
    }
    const auto cancellationRequested = [cancellation]() {
      return cancellation && cancellation->requested();
    };
    if (!prepareOptionsBrowserContent(state, optionsRuntime,
                                      cancellationRequested)) {
      if (cancellation && cancellation->requested()) {
        return BrowserPreparationCancelled{};
      }
      return browserPreparationError(
          BrowserPreparationErrorKind::Unavailable, state.location,
          "Unable to prepare media options.");
    }
  } else if (isTrackBrowserActive(state)) {
    std::string trackError;
    std::shared_ptr<const TrackBrowserContent> content =
        prepareTrackBrowserContent(state.location.path(), &trackError);
    if (cancellation && cancellation->requested()) {
      return BrowserPreparationCancelled{};
    }
    if (!content) {
      return browserPreparationError(
          BrowserPreparationErrorKind::Unavailable, state.location,
          trackError.empty() ? "Unable to prepare the track browser."
                             : std::move(trackError));
    }
    state.content = content;
    state.entries.clear();
    if (content->file.has_parent_path()) {
      state.entries.emplace_back("..", content->file.parent_path(),
                                 browser_entry::NavigateUp{});
    }
    int digits = trackLabelDigits(content->tracks.size());
    for (const auto& track : content->tracks) {
      BrowserEntry entry{formatTrackLabel(track, digits), content->file,
                         browser_entry::PlayTrack{track.index}};
      entry.pathIdentity = content->fileIdentity;
      state.entries.push_back(std::move(entry));
    }
  } else {
    DirectoryListingResult listing = listEntries(state.location, cancellation);
    if (auto* error = std::get_if<BrowserPreparationError>(&listing)) {
      return std::move(*error);
    }
    if (std::holds_alternative<BrowserPreparationCancelled>(listing)) {
      return BrowserPreparationCancelled{};
    }
    state.content = std::monostate{};
    state.entries =
        std::move(std::get<std::vector<BrowserEntry>>(listing));
  }

  if (!state.filter.empty()) {
    std::string lowFilter = toLower(state.filter);
    state.entries.erase(
        std::remove_if(state.entries.begin(), state.entries.end(),
                       [&](const BrowserEntry& e) {
                         if (e.isSectionHeader() || e.isStatus() ||
                             e.actionAs<browser_entry::NavigateUp>()) {
                           return false;
                         }
                         return toLower(e.name).find(lowFilter) ==
                                std::string::npos;
                       }),
        state.entries.end());
  }

  if (cancellation && cancellation->requested()) {
    return BrowserPreparationCancelled{};
  }

  if (!state.entries.empty() && !optionsActive) {
    sortBrowserEntries(state);
  }

  if (cancellation && cancellation->requested()) {
    return BrowserPreparationCancelled{};
  }

  if (state.entries.empty()) {
    state.selected = 0;
    state.scrollRow = 0;
    return std::monostate{};
  }

  auto isSelectable = [](const BrowserEntry& entry) {
    return entry.isSelectable();
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
      if (!state.entries[i].isSelectable()) continue;
      if (toLower(state.entries[i].name) == toLower(initialName)) {
        state.selected = static_cast<int>(i);
        requestBrowserSelectionReveal(state);
        break;
      }
    }
  }
  return std::monostate{};
}

static std::optional<BrowserPreparationResult> prepareBrowserContent(
    const BrowserContentRequest& request,
    const BrowserContentWorker::Cancellation* cancellation = nullptr) {
  BrowserState candidate;
  candidate.location = request.location;
  candidate.content = request.previousContent;
  candidate.selected = request.selected;
  candidate.sortMode = request.sortMode;
  candidate.sortDescending = request.sortDescending;
  candidate.filter = request.filter;
  BrowserPopulationResult population =
      populateBrowser(candidate, request.initialName, request.optionsRuntime,
                      cancellation);
  if (std::holds_alternative<BrowserPreparationCancelled>(population)) {
    return std::nullopt;
  }
  if (auto* error = std::get_if<BrowserPreparationError>(&population)) {
    return BrowserPreparationResult(std::move(*error));
  }

  PreparedBrowserContent prepared;
  prepared.entries = std::move(candidate.entries);
  prepared.content = std::move(candidate.content);
  prepared.selected = candidate.selected;
  prepared.scrollRow = candidate.scrollRow;
  prepared.viewportRestoreMode = candidate.viewportRestoreMode;
  prepared.viewportRestoreScrollRow = candidate.viewportRestoreScrollRow;
  return BrowserPreparationResult(std::move(prepared));
}

static std::string buildTrackSelectionMeta(const BrowserState& browser) {
  if (browser.entries.empty()) return "";
  int idx = std::clamp(browser.selected, 0,
                       static_cast<int>(browser.entries.size()) - 1);
  const auto& entry = browser.entries[static_cast<size_t>(idx)];
  std::string name = entry.name;
  if (entry.isDirectory() && !entry.actionAs<browser_entry::NavigateUp>()) {
    name += "/";
  }

  std::string sortLabel = "Name";
  if (browser.sortMode == BrowserState::SortMode::Date) sortLabel = "Date";
  else if (browser.sortMode == BrowserState::SortMode::Size) sortLabel = "Size";

  std::string dirArrow = browser.sortDescending ? " \xE2\x86\x93" : " \xE2\x86\x91";
  std::string metaLine = " [" + sortLabel + dirArrow + "]";

  metaLine += " Selected: " + name;
  if (const auto* selectedTrack =
          entry.actionAs<browser_entry::PlayTrack>()) {
    const TrackBrowserContent* content = trackBrowserContent(browser);
    const TrackEntry* track =
        findTrackEntry(browser, selectedTrack->trackIndex);
    if (track && track->lengthMs > 0) {
      metaLine += "  " + formatTime(static_cast<double>(track->lengthMs) / 1000.0);
    }
    if (content && !content->tracks.empty()) {
      metaLine += "  Track " +
                  std::to_string(selectedTrack->trackIndex + 1) + "/" +
                  std::to_string(content->tracks.size());
    }
  }
  return metaLine;
}

static std::optional<image_viewer_sequence::Sequence>
imageSequenceFromFiles(const std::vector<std::filesystem::path>& files,
                       const std::filesystem::path& current) {
  std::vector<std::filesystem::path> images;
  images.reserve(files.size());
  for (const std::filesystem::path& file : files) {
    if (isSupportedImageExt(file)) {
      images.push_back(file);
    }
  }
  return image_viewer_sequence::Sequence::create(std::move(images), current);
}

static std::optional<std::filesystem::path> openDirectoryFromFiles(
    const std::vector<std::filesystem::path>& files) {
  for (const std::filesystem::path& file : files) {
    std::error_code ec;
    if (std::filesystem::is_directory(file, ec) && !ec) {
      return file;
    }
  }
  return std::nullopt;
}

static std::vector<std::filesystem::path> imageFilesFromBrowserEntries(
    const std::vector<BrowserEntry>& entries) {
  std::vector<std::filesystem::path> images;
  images.reserve(entries.size());
  for (const BrowserEntry& entry : entries) {
    if (entry.actionAs<browser_entry::OpenFile>() &&
        isSupportedImageExt(entry.path)) {
      images.push_back(entry.path);
    }
  }
  return images;
}

enum class MediaCommandFailureKind : std::uint8_t {
  Busy,
  Unsupported,
  QueueUnavailable,
  PlaybackFailed,
  NavigationFailed,
};

struct MediaCommandFailure {
  MediaCommandFailureKind kind;
  std::string message;
};

class MediaCommandResult {
 public:
  enum class Status : std::uint8_t {
    Applied,
    Deferred,
    HandledWithoutPlayback,
    Rejected,
  };

  static MediaCommandResult applied() {
    return MediaCommandResult(Status::Applied);
  }
  static MediaCommandResult deferred() {
    return MediaCommandResult(Status::Deferred);
  }
  static MediaCommandResult handledWithoutPlayback() {
    return MediaCommandResult(Status::HandledWithoutPlayback);
  }
  static MediaCommandResult rejected(MediaCommandFailure failure) {
    return MediaCommandResult(std::move(failure));
  }

  Status status() const { return status_; }
  bool accepted() const { return status_ != Status::Rejected; }
  const MediaCommandFailure* failure() const {
    return failure_ ? &*failure_ : nullptr;
  }

 private:
  explicit MediaCommandResult(Status status) : status_(status) {}
  explicit MediaCommandResult(MediaCommandFailure failure)
      : status_(Status::Rejected), failure_(std::move(failure)) {}

  Status status_;
  std::optional<MediaCommandFailure> failure_;
};

class TuiMediaCoordinator {
 public:
  struct Services {
    playback_queue::Queue& queue;
    ConsoleInput& input;
    ConsoleScreen& screen;
    const Style& baseStyle;
    const Style& accentStyle;
    const Style& dimStyle;
    const Style& progressEmptyStyle;
    const Style& progressFrameStyle;
    const Color& progressStart;
    const Color& progressEnd;
    const VideoPlaybackConfig& videoConfig;
    OpenFileRequests& openFileRequests;
    std::function<bool(const std::filesystem::path&, int)> startAudio;
    std::function<void(playback_route::AudioPictureInPicturePlan)>
        applyAudioPictureInPicturePlan;
    std::function<bool(const std::filesystem::path&)> openBrowserDirectory;
    std::function<void(std::string)> setCommandError;
    std::function<void()> requestQuit;
    std::function<void()> presentationFinished;
    playback_media_processing::Actions mediaProcessingActions;
    std::function<void()> activateBrowserSurface;
  };

  explicit TuiMediaCoordinator(Services services)
      : services_(std::move(services)) {}

  [[nodiscard]] MediaCommandResult startPlayback(
      playback_route::Route route, playback_queue::Source source) {
    std::optional<playback_queue::Queue::PreparedActivation> activation =
        services_.queue.prepareStart(std::move(route), std::move(source));
    if (!activation) {
      return reject(MediaCommandFailureKind::QueueUnavailable,
                    "Unable to prepare the playback queue.");
    }
    return submit(PreparedPlayback{std::move(*activation)});
  }

  [[nodiscard]] MediaCommandResult startFiles(
      playback_route::Route route,
      const std::vector<std::filesystem::path>& files) {
    CommandBuildResult command =
        mediaCommandFromFiles(std::move(route), files);
    if (auto* failure = std::get_if<MediaCommandFailure>(&command)) {
      return reject(std::move(*failure));
    }
    return submit(std::move(std::get<Command>(command)));
  }

  [[nodiscard]] MediaCommandResult startDroppedFiles(
      const std::vector<std::filesystem::path>& files,
      const WindowPlacementState* sourcePlacement,
      std::optional<PlaybackPresentationState> videoPresentation) {
    std::optional<playback_route::Route> route =
        playback_route::resolveDroppedTarget(files, sourcePlacement,
                                             videoPresentation);
    if (!route) {
      return reject(MediaCommandFailureKind::Unsupported,
                    "No supported media item was found in the drop request.");
    }
    return startFiles(std::move(*route), files);
  }

  [[nodiscard]] MediaCommandResult openFiles(const OpenFilesRequest& request) {
    CommandBuildResult command = commandFromOpenFiles(request);
    if (auto* failure = std::get_if<MediaCommandFailure>(&command)) {
      return reject(std::move(*failure));
    }
    return submit(std::move(std::get<Command>(command)));
  }

  [[nodiscard]] MediaCommandResult transport(
      playback_queue::Direction direction) {
    std::optional<playback_queue::Queue::PreparedActivation> activation =
        services_.queue.prepareTransport(direction);
    if (!activation) {
      return reject(MediaCommandFailureKind::QueueUnavailable, {});
    }
    return submit(PreparedPlayback{std::move(*activation)});
  }

  std::optional<MediaCommandResult> pump() {
    if (!videoSession_) return std::nullopt;
    std::optional<PlaybackSessionCompletion> completion =
        videoSession_->pump();
    if (!completion) return std::nullopt;
    finishVideoSession(std::move(*completion));
    return drainPendingCommands();
  }

  bool videoActive() const { return videoSession_.has_value(); }

  PlaybackShellTerminalRole terminalRole() const {
    return videoSession_ ? videoSession_->terminalRole()
                         : PlaybackShellTerminalRole::Browser;
  }

  std::filesystem::path currentPlaybackFile() const {
    return videoTarget_ ? playbackTargetFile(*videoTarget_)
                        : audioGetNowPlaying();
  }

  std::optional<int> currentPlaybackTrackIndex() const {
    if (videoTarget_) {
      return playbackTargetTrackIndex(*videoTarget_);
    }
    const int trackIndex = audioGetTrackIndex();
    return trackIndex >= 0 ? std::optional<int>(trackIndex) : std::nullopt;
  }

  std::vector<NativeWaitHandle> activityWaitHandles() const {
    return videoSession_ ? videoSession_->activityWaitHandles()
                         : std::vector<NativeWaitHandle>{};
  }

  int nextWakeTimeoutMs() const {
    return videoSession_ ? videoSession_->nextWakeTimeoutMs() : 250;
  }

  std::optional<PlaybackControlState> videoControlState() const {
    return videoSession_
               ? std::optional<PlaybackControlState>(
                     videoSession_->controlState())
               : std::nullopt;
  }

  std::optional<PlaybackPresentationState> videoPresentationState() const {
    return videoSession_
               ? std::optional<PlaybackPresentationState>(
                     videoSession_->presentationState())
               : std::nullopt;
  }

  bool capturesBrowserInput() const {
    return videoSession_ && videoSession_->capturesBrowserInput();
  }

  bool handleVideoInputEvent(const InputEvent& event) {
    return videoSession_ && videoSession_->handleInputEvent(event);
  }

  bool handleControlCommand(PlaybackControlCommand command) {
    return videoSession_ && videoSession_->handleControlCommand(command);
  }

  bool seekVideoToRatio(double ratio) {
    return videoSession_ && videoSession_->seekToRatio(ratio);
  }

  bool toggleWindowPresentation() {
    return videoSession_ && videoSession_->toggleWindowPresentation();
  }

  bool togglePictureInPicture() {
    return videoSession_ && videoSession_->togglePictureInPicture();
  }

  bool toggleFullscreen() {
    return videoSession_ && videoSession_->toggleFullscreen();
  }

  bool activateVideoPresentation() {
    return videoSession_ && videoSession_->activatePresentation();
  }

  void subtitleGenerationFinishedFor(
      const std::filesystem::path& sourceFile,
      const std::filesystem::path& outputFile, bool success,
      std::string status) {
    if (!videoSession_ || !videoTarget_ ||
        !samePath(playbackTargetFile(*videoTarget_), sourceFile)) {
      return;
    }
    videoSession_->subtitleGenerationFinished(outputFile, success,
                                              std::move(status));
  }

  void mediaTaskFinishedFor(const std::filesystem::path& sourceFile,
                            std::string status) {
    if (!videoSession_ || !videoTarget_ ||
        !samePath(playbackTargetFile(*videoTarget_), sourceFile)) {
      return;
    }
    videoSession_->mediaTaskFinished(std::move(status));
  }

  void stopVideo() {
    if (videoSession_) videoSession_->requestStop();
  }

  void requestQuit() {
    if (videoSession_) {
      videoSession_->requestQuit();
    } else {
      services_.requestQuit();
    }
  }

 private:
  struct PreparedPlayback {
    playback_queue::Queue::PreparedActivation activation;
  };

  struct ImageActivation {
    playback_route::Route route;
    image_viewer_sequence::Sequence sequence;
  };

  struct OpenDirectory {
    std::filesystem::path path;
  };

  struct Quit {};

  using Command =
      std::variant<PreparedPlayback, ImageActivation, OpenDirectory, Quit>;
  using CommandBuildResult = std::variant<Command, MediaCommandFailure>;

  class DriveScope {
   public:
    explicit DriveScope(bool& driving) : driving_(driving) { driving_ = true; }
    ~DriveScope() { driving_ = false; }

    DriveScope(const DriveScope&) = delete;
    DriveScope& operator=(const DriveScope&) = delete;

   private:
    bool& driving_;
  };

  CommandBuildResult mediaCommandFromFiles(
      playback_route::Route route,
      const std::vector<std::filesystem::path>& files) const {
    const std::filesystem::path& targetFile =
        playbackTargetFile(route.target);
    if (isSupportedImageExt(targetFile)) {
      std::optional<image_viewer_sequence::Sequence> sequence =
          imageSequenceFromFiles(files, targetFile);
      if (!sequence) {
        return MediaCommandFailure{
            MediaCommandFailureKind::Unsupported,
            "The selected images do not form a viewable sequence."};
      }
      return Command(ImageActivation{std::move(route), std::move(*sequence)});
    }
    std::optional<playback_queue::Queue::PreparedActivation> activation =
        services_.queue.prepareStart(std::move(route),
                                     playback_queue::sourceFromFiles(files));
    if (!activation) {
      return MediaCommandFailure{
          MediaCommandFailureKind::QueueUnavailable,
          "Unable to prepare the playback queue."};
    }
    return Command(PreparedPlayback{std::move(*activation)});
  }

  CommandBuildResult commandFromOpenFiles(
      const OpenFilesRequest& request) const {
    if (std::optional<std::filesystem::path> directory =
            openDirectoryFromFiles(request.files)) {
      return Command(OpenDirectory{std::move(*directory)});
    }
    std::optional<playback_route::Route> route =
        resolveOpenFilesPlaybackRoute(request,
                                      services_.videoConfig.enableAscii);
    if (!route) {
      return MediaCommandFailure{
          MediaCommandFailureKind::Unsupported,
          "No supported media item was found in the open request."};
    }
    return mediaCommandFromFiles(std::move(*route), request.files);
  }

  MediaCommandResult enqueueOpenFiles(const OpenFilesRequest& request) {
    if (pendingCommand_) {
      return reject(MediaCommandFailureKind::Busy, {});
    }
    CommandBuildResult command = commandFromOpenFiles(request);
    if (auto* failure = std::get_if<MediaCommandFailure>(&command)) {
      return reject(std::move(*failure));
    }
    pendingCommand_.emplace(std::move(std::get<Command>(command)));
    clearCommandError();
    return MediaCommandResult::deferred();
  }

  void enqueueQuit() {
    if (!pendingCommand_) {
      pendingCommand_.emplace(Quit{});
    }
  }

  MediaCommandResult requestVideoHandoff(Command command) {
    if (!videoSession_ || pendingCommand_ || handoffCommand_) {
      return reject(MediaCommandFailureKind::Busy, {});
    }
    handoffCommand_.emplace(std::move(command));
    const bool requested = videoSession_->requestHandoff([this](bool accepted) {
      if (!handoffCommand_) return;
      if (accepted && !pendingCommand_) {
        pendingCommand_.emplace(std::move(*handoffCommand_));
      }
      handoffCommand_.reset();
    });
    if (!requested) {
      handoffCommand_.reset();
      return reject(MediaCommandFailureKind::Busy, {});
    }
    clearCommandError();
    return MediaCommandResult::deferred();
  }

  MediaCommandResult submit(Command command) {
    if (videoSession_) {
      return requestVideoHandoff(std::move(command));
    }
    if (driving_) {
      if (pendingCommand_) {
        return reject(MediaCommandFailureKind::Busy, {});
      }
      pendingCommand_.emplace(std::move(command));
      clearCommandError();
      return MediaCommandResult::deferred();
    }
    return drive(std::move(command));
  }

  MediaCommandResult drive(Command initialCommand) {
    if (driving_) {
      return reject(MediaCommandFailureKind::Busy, {});
    }

    DriveScope driveScope(driving_);
    std::optional<Command> command(std::move(initialCommand));
    MediaCommandResult result = MediaCommandResult::handledWithoutPlayback();
    while (command) {
      result = dispatch(std::move(*command));
      if (!result.accepted()) {
        pendingCommand_.reset();
        publishFailure(result);
        return result;
      }
      if (videoSession_) break;
      command = std::exchange(pendingCommand_, std::nullopt);
    }
    clearCommandError();
    return result;
  }

  std::optional<MediaCommandResult> drainPendingCommands() {
    if (videoSession_ || driving_ || !pendingCommand_) return std::nullopt;
    Command command = std::move(*pendingCommand_);
    pendingCommand_.reset();
    return drive(std::move(command));
  }

  static VideoPlaybackConfig sessionConfig(
      const VideoPlaybackConfig& base,
      const PlaybackSessionContinuationState& continuation) {
    VideoPlaybackConfig config = base;
    if (continuation.presentation) {
      config.enableAscii = continuation.presentation->usesAsciiGrid();
    }
    return config;
  }

  static playback_queue::Direction transportDirection(
      PlaybackTransportCommand command) {
    return command == PlaybackTransportCommand::Previous
               ? playback_queue::Direction::Previous
               : playback_queue::Direction::Next;
  }

  MediaCommandResult presentPlayback(
      playback_queue::Queue::PreparedActivation activation) {
    const playback_route::Route& route = activation.route();
    if (route.videoContinuation) {
      continuationState_ = *route.videoContinuation;
    }
    services_.applyAudioPictureInPicturePlan(route.audioPictureInPicture);

    const PlaybackTarget target = route.target;
    const std::filesystem::path& targetFile = playbackTargetFile(target);
    if (const std::optional<int> trackIndex =
            playbackTargetTrackIndex(target)) {
      if (!services_.startAudio(targetFile, *trackIndex)) {
        return MediaCommandResult::rejected(
            {MediaCommandFailureKind::PlaybackFailed, {}});
      }
      services_.queue.commit(std::move(activation));
      return MediaCommandResult::applied();
    }
    if (isSupportedImageExt(targetFile)) {
      return MediaCommandResult::rejected(
          {MediaCommandFailureKind::Unsupported,
           "The image request did not contain an image sequence."});
    }
    if (!isSupportedVideoExt(targetFile)) {
      if (!services_.startAudio(targetFile, 0)) {
        return MediaCommandResult::rejected(
            {MediaCommandFailureKind::PlaybackFailed, {}});
      }
      services_.queue.commit(std::move(activation));
      return MediaCommandResult::applied();
    }

    auto requestTransport = [this](PlaybackTransportCommand command) {
      if (!videoSession_ || pendingCommand_) {
        return false;
      }
      std::optional<playback_queue::Queue::PreparedActivation> successor =
          services_.queue.prepareTransport(transportDirection(command));
      if (!successor) {
        return false;
      }
      pendingCommand_.emplace(PreparedPlayback{std::move(*successor)});
      return true;
    };
    auto requestDroppedFiles =
        [this](const std::vector<std::filesystem::path>& files) {
          OpenFilesRequest request;
          request.files = files;
          return enqueueOpenFiles(request).accepted();
        };

    PlaybackSession::Request sessionRequest{
        targetFile,
        sessionConfig(services_.videoConfig, continuationState_),
        continuationState_,
        route.sessionIntent,
        std::move(requestTransport),
        std::move(requestDroppedFiles),
        services_.mediaProcessingActions,
        services_.activateBrowserSurface};
    PlaybackSession::Dependencies sessionDependencies{
        services_.input,
        services_.screen,
        services_.baseStyle,
        services_.accentStyle,
        services_.dimStyle,
        services_.progressEmptyStyle,
        services_.progressFrameStyle,
        services_.progressStart,
        services_.progressEnd};
    videoSession_.emplace(std::move(sessionRequest),
                          std::move(sessionDependencies));

    const PlaybackSessionOpenOutcome openOutcome = videoSession_->open();
    if (openOutcome == PlaybackSessionOpenOutcome::Ready) {
      services_.queue.commit(std::move(activation));
      videoTarget_ = target;
      services_.presentationFinished();
      return MediaCommandResult::applied();
    } else if (openOutcome ==
               PlaybackSessionOpenOutcome::AudioFallbackRequested) {
      videoSession_.reset();
      if (!services_.startAudio(targetFile, 0)) {
        return MediaCommandResult::rejected(
            {MediaCommandFailureKind::PlaybackFailed, {}});
      }
      services_.queue.commit(std::move(activation));
      return MediaCommandResult::applied();
    } else if (openOutcome ==
               PlaybackSessionOpenOutcome::QuitApplicationRequested) {
      videoSession_.reset();
      enqueueQuit();
      services_.presentationFinished();
      return MediaCommandResult::handledWithoutPlayback();
    }
    videoSession_.reset();
    services_.presentationFinished();
    return MediaCommandResult::handledWithoutPlayback();
  }

  void finishVideoSession(PlaybackSessionCompletion completion) {
    continuationState_ = std::move(completion.continuityState);
    videoSession_.reset();
    videoTarget_.reset();
    if (completion.intent == PlaybackSessionExitIntent::QuitApplication) {
      enqueueQuit();
    }
    services_.presentationFinished();
  }

  MediaCommandResult dispatch(PreparedPlayback playback) {
    return presentPlayback(std::move(playback.activation));
  }

  MediaCommandResult dispatch(ImageActivation image) {
    services_.applyAudioPictureInPicturePlan(image.route.audioPictureInPicture);
    const image_viewer::Exit exit = image_viewer::run(
        std::move(image.sequence), services_.input, services_.screen,
        services_.baseStyle, services_.accentStyle, services_.dimStyle,
        services_.openFileRequests, [this](const OpenFilesRequest& request) {
          return enqueueOpenFiles(request).accepted();
        });
    if (exit == image_viewer::Exit::QuitRequested) {
      enqueueQuit();
    }
    services_.presentationFinished();
    return MediaCommandResult::applied();
  }

  MediaCommandResult dispatch(OpenDirectory directory) {
    if (!services_.openBrowserDirectory(directory.path)) {
      return MediaCommandResult::rejected(
          {MediaCommandFailureKind::NavigationFailed,
           "Unable to open the requested folder."});
    }
    return MediaCommandResult::applied();
  }

  MediaCommandResult dispatch(Quit) {
    services_.requestQuit();
    return MediaCommandResult::applied();
  }

  MediaCommandResult dispatch(Command command) {
    return std::visit(
        [this](auto action) { return dispatch(std::move(action)); },
        std::move(command));
  }

  MediaCommandResult reject(MediaCommandFailureKind kind,
                            std::string message) {
    return reject(MediaCommandFailure{kind, std::move(message)});
  }

  MediaCommandResult reject(MediaCommandFailure failure) {
    MediaCommandResult result =
        MediaCommandResult::rejected(std::move(failure));
    publishFailure(result);
    return result;
  }

  void publishFailure(const MediaCommandResult& result) {
    const MediaCommandFailure* failure = result.failure();
    if (failure && !failure->message.empty() && services_.setCommandError) {
      services_.setCommandError(failure->message);
    }
  }

  void clearCommandError() {
    if (services_.setCommandError) services_.setCommandError({});
  }

  Services services_;
  PlaybackSessionContinuationState continuationState_;
  std::optional<PlaybackSession> videoSession_;
  std::optional<PlaybackTarget> videoTarget_;
  std::optional<Command> pendingCommand_;
  std::optional<Command> handoffCommand_;
  bool driving_ = false;
};

int runTui(Options o, playback_queue::Queue& playbackQueue) {
  const ShellOpenMode shellOpenMode = resolveWindowsShellOpenMode(o);
  const bool acceptShellOpenHandoffs =
      shellOpenMode == ShellOpenMode::SameInstance;
  const bool forwardInputToExistingInstance =
      shouldForwardInputToExistingInstance(o, shellOpenMode);

  if ((o.shellOpen || forwardInputToExistingInstance) && !o.input.empty()) {
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
  audioConfig.radioReceiverProfile = o.radioReceiverProfile;
  audioConfig.radioReceptionProfile = o.radioReceptionProfile;
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

  ConsoleInput input;
  OpenFileRequests openFileRequests;
  std::optional<WindowsShellOpenServer> shellOpenServer;
  if (acceptShellOpenHandoffs) {
    shellOpenServer.emplace(openFileRequests);
    const bool acceptingShellOpenHandoffs =
        shellOpenServer->isAcceptingHandoffs();
    if (!acceptingShellOpenHandoffs && forwardInputToExistingInstance &&
        !o.input.empty()) {
      const OpenPresentationDirective presentation =
          o.shellOpenPresentationOverride.value_or(
              OpenPresentationDirective::InheritActive);
      if (forwardWindowsShellOpenFile(pathFromUtf8String(o.input),
                                      presentation)) {
        return 0;
      }
    }
  }

  windows_file_drop::OleApartment tuiWindowThreadApartment;

  float lpHz = static_cast<float>(o.bwHz);
  const uint32_t sampleRate = 48000;

  auto radio1938Template = std::make_unique<Radio1938>();
  radio1938Template->applyReceiverProfile(o.radioReceiverProfile);
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

  bool dirty = true;
  UiDirtyFlags dirtyFlags = UiDirtyFlags::Frame | UiDirtyFlags::Layout;
  bool layoutDirty = true;
  bool forceFullRedraw = true;
  bool screenSizeDirty = true;
  auto markDirty = [&](UiDirtyFlags flags = UiDirtyFlags::Frame) {
    dirty = true;
    dirtyFlags |= flags;
    if (hasDirtyFlag(flags, UiDirtyFlags::Layout)) {
      layoutDirty = true;
      forceFullRedraw = true;
    }
  };
  auto markLayoutDirty = [&]() {
    markDirty(UiDirtyFlags::Frame | UiDirtyFlags::Layout);
  };

  BrowserState browser;
  browser.location = browserDirectoryLocation({});
  bool capturingInitialBrowserPreparation = true;
  std::optional<BrowserPreparationId> initialBrowserPreparationId;
  BrowserContentWorker browserContentWorker(
      [](BrowserContentRequest request,
         const BrowserContentWorker::Cancellation& cancellation) {
        try {
          return prepareBrowserContent(request, &cancellation);
        } catch (const std::exception& error) {
          return std::optional<BrowserPreparationResult>(
              BrowserPreparationError{
                  BrowserPreparationErrorKind::Internal, request.location,
                  error.what()});
        } catch (...) {
          return std::optional<BrowserPreparationResult>(
              BrowserPreparationError{
                  BrowserPreparationErrorKind::Internal, request.location,
                  "Unexpected browser preparation failure."});
        }
      });
  BrowserNavigator::Callbacks browserNavigationCallbacks;
  browserNavigationCallbacks.prepare =
      [&](BrowserPreparationId preparationId,
          const BrowserContentRequest& request) {
        if (capturingInitialBrowserPreparation) {
          initialBrowserPreparationId = preparationId;
        }
        BrowserContentRequest workerRequest = request;
        if (request.location.kind() == BrowserLocationKind::OptionsBrowser) {
          workerRequest.optionsRuntime = captureOptionsBrowserRuntimeSnapshot(
              request.location, sampleRate, audioConfig.mono ? 1u : 2u);
        }
        if (!browserContentWorker.submit(preparationId,
                                         std::move(workerRequest))) {
          return BrowserContentPreparation::failed(BrowserPreparationError{
              BrowserPreparationErrorKind::Internal, request.location,
              "The browser preparation worker is unavailable."});
        }
        return BrowserContentPreparation::pending();
      };
  browserNavigationCallbacks.cancelPreparation =
      [&](BrowserPreparationId cancellationId) {
        browserContentWorker.cancel(cancellationId);
      };
  browserNavigationCallbacks.changed = [&]() { markLayoutDirty(); };
  BrowserNavigator browserNavigator(browser,
                                    std::move(browserNavigationCallbacks));
  BrowserSelectionMetadata browserSelectionMetadata(isVideoExt);
  const bool initialBrowserPreparationAccepted =
      browserNavigator.initialize(browserDirectoryLocation(startDir),
                                  initialName);
  capturingInitialBrowserPreparation = false;
  if (!initialBrowserPreparationAccepted) {
    initialBrowserPreparationId.reset();
    browserNavigator.initialize(browserDirectoryLocation({}));
  }
  input.init();

  ConsoleScreen screen;
  screen.init();
  input.enableTerminalMouseInput();

  VideoWindow tuiWindow;
  bool windowTuiEnabled = o.enableWindow;
  if (windowTuiEnabled) {
    const WindowClientSize clientSize = initialWindowTuiClientSize(screen);
    if (!tuiWindow.Open(clientSize.width, clientSize.height,
                        RADIOIFY_APP_NAME)) {
      windowTuiEnabled = false;
    } else {
      tuiWindow.EnableFileDrop();
      tuiWindow.SetCaptureAllMouseInput(true);
      tuiWindow.SetVsync(true);
      if (!tuiWindow.Show(VideoWindowFocus::TakeForegroundFocus)) {
        tuiWindow.Close();
        windowTuiEnabled = false;
      }
    }
  }

  audioInit(audioConfig);
  PlaybackSystemControls systemControls;
  systemControls.initialize();
  PlaybackNotificationAreaControls notificationAreaControls;
  notificationAreaControls.initialize();

  VideoPlaybackConfig videoConfig;
  videoConfig.enableAscii = o.enableAscii;
  videoConfig.enableAudio = o.enableAudio;
  videoConfig.debugOverlay = o.asciiDebugOverlay;

  std::optional<OpenFilesRequest> initialOpenRequest;

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
    logLine(RADIOIFY_APP_NAME);
    logLine(std::string("  Mode:   render"));
    logLine(std::string("  Input:  ") + toUtf8String(file));
    logLine(std::string("  Output: ") + toUtf8String(outputPath));
    logLine("Rendering output...");
    Radio1938 activeRadioTemplate = *radio1938Template;
    const RadioFilterMode activeMode = audioGetRadioFilterMode();
    if (radioFilterModeEnabled(activeMode) &&
        activeRadioTemplate.receiverProfile !=
            radioFilterModeReceiverProfile(activeMode)) {
      const RadioReceiverProfile activeProfile =
          radioFilterModeReceiverProfile(activeMode);
      activeRadioTemplate.applyReceiverProfile(activeProfile);
      if (!o.radioSettingsPath.empty()) {
        std::string radioIniError;
        if (!applyRadioSettingsIni(activeRadioTemplate, o.radioSettingsPath,
                                   o.radioPresetName, &radioIniError)) {
          die("Failed to apply radio settings from '" +
              o.radioSettingsPath + "': " + radioIniError);
        }
      }
    }
    renderToFile(renderOpt, file, outputPath, activeRadioTemplate,
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
  const Style kStyleBrowserHover{{215, 220, 226}, {35, 43, 54}};
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
          "to radioify.exe, or in " RADIOIFY_APP_NAME "'s launch directory.";
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

  if (!o.input.empty() && o.play) {
    std::filesystem::path inputPath = pathFromUtf8String(o.input);
    if (std::filesystem::exists(inputPath)) {
      if (!std::filesystem::is_directory(inputPath)) {
        OpenFilesRequest request;
        request.files.push_back(std::move(inputPath));
        request.presentation = o.shellOpenPresentationOverride.value_or(
            OpenPresentationDirective::UseLaunchDefaults);
        initialOpenRequest.emplace(std::move(request));
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
  std::optional<int> lastMelodyTrackIndex;
  bool lastMelodyAnalysisRunning = false;
  std::vector<ScreenCell> windowCells;
  AudioPictureInPictureWindow audioPictureInPicture;
  ConsoleInputPump consoleInputPump;
  pointer_input::MouseDoubleClickTracker browserDoubleClickTracker;
  browser_input::EntryClickTracker browserEntryClickTracker;
  BrowserPointerState browserPointerState;
  BrowserViewport viewport;
  BrowserFooterLayout footerLayout;
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
  std::function<std::filesystem::path()> currentPlaybackFile =
      []() { return audioGetNowPlaying(); };
  std::function<std::optional<int>()> currentPlaybackTrackIndex = []() {
    const int trackIndex = audioGetTrackIndex();
    return trackIndex >= 0 ? std::optional<int>(trackIndex) : std::nullopt;
  };
  auto buildNowPlayingLabel = [&]() {
    std::filesystem::path nowPlaying = currentPlaybackFile();
    std::string label =
        nowPlaying.empty() ? std::string("(none)")
                           : toUtf8String(nowPlaying.filename());
    const std::optional<int> trackIndex = currentPlaybackTrackIndex();
    if (!nowPlaying.empty() && trackIndex) {
      int digits = 3;
      const TrackEntry* track = nullptr;
      const TrackBrowserContent* content = trackBrowserContent(browser);
      if (content && samePath(nowPlaying, content->file) &&
          !content->tracks.empty()) {
        digits = trackLabelDigits(content->tracks.size());
        track = findTrackEntry(browser, *trackIndex);
      }
      label += "  |  " +
               (track ? formatTrackLabel(*track, digits)
                      : formatTrackIndexLabel(*trackIndex, digits));
    }
    return label;
  };
  auto showAudioPictureInPictureOpenError = [&]() {
    const std::string detail = audioPictureInPicture.lastError().empty()
                                   ? "The picture-in-picture window did not open."
                                   : audioPictureInPicture.lastError();
    playback_dialog::showInfoDialog(
        input, screen, kStyleNormal, kStyleAccent, kStyleDim,
        "Picture-in-Picture Error",
        RADIOIFY_APP_NAME " could not open picture-in-picture.",
        detail, "Enter/Space/Esc: close");
  };
  auto applyAudioPictureInPicturePlan =
      [&](playback_route::AudioPictureInPicturePlan plan) {
    switch (plan) {
      case playback_route::AudioPictureInPicturePlan::Keep:
        break;
      case playback_route::AudioPictureInPicturePlan::Close:
        if (audioPictureInPicture.isOpen()) {
          audioPictureInPicture.close();
          markDirty(UiDirtyFlags::Async);
        }
        break;
    }
  };

  BrowserPlaybackRevealer::Callbacks browserPlaybackCallbacks;
  browserPlaybackCallbacks.markDirty = [&]() { markDirty(); };
  browserPlaybackCallbacks.markLayoutDirty = [&]() { markLayoutDirty(); };
  BrowserPlaybackRevealer browserPlaybackRevealer(
      browserNavigator, std::move(browserPlaybackCallbacks));

  media_processing::Coordinator mediaTasks;
  media_processing::PlaybackService mediaTaskPlaybackService(
      mediaTasks, [&]() {
        markLayoutDirty();
        markDirty(UiDirtyFlags::Async);
      });
  playback_media_processing::Actions mediaProcessingActions(
      mediaTaskPlaybackService);
  auto cancelActiveMediaTask = [&]() {
    return mediaTaskPlaybackService.requestActiveCancellation();
  };

  std::string mediaCommandError;
  auto openBrowserDirectory = [&](const std::filesystem::path& dir) {
    return browserNavigator.navigate(browserDirectoryLocation(dir));
  };

  TuiMediaCoordinator mediaCoordinator(
      {playbackQueue, input, screen, kStyleNormal, kStyleAccent, kStyleDim,
       kStyleProgressEmpty, kStyleProgressFrame, kProgressStart, kProgressEnd,
       videoConfig, openFileRequests,
       [&](const std::filesystem::path& file, int trackIndex) {
         return tryStartAudioFile(file, trackIndex);
       },
       applyAudioPictureInPicturePlan, openBrowserDirectory,
       [&](std::string error) {
         mediaCommandError = std::move(error);
         markDirty(UiDirtyFlags::Async);
       },
       [&]() { running = false; }, [&]() { markDirty(); },
       mediaProcessingActions,
       [&]() {
         if (windowTuiEnabled && tuiWindow.IsOpen()) {
           tuiWindow.Activate();
         } else {
           activateWindowsConsoleWindow();
         }
       }});
  currentPlaybackFile =
      [&]() { return mediaCoordinator.currentPlaybackFile(); };
  auto mediaActivityWaitHandles = [&]() {
    std::vector<NativeWaitHandle> handles =
        mediaCoordinator.activityWaitHandles();
    std::vector<NativeWaitHandle> taskHandles = mediaTasks.waitHandles();
    handles.insert(handles.end(), taskHandles.begin(), taskHandles.end());
    return handles;
  };
  currentPlaybackTrackIndex =
      [&]() { return mediaCoordinator.currentPlaybackTrackIndex(); };
  auto currentPlaybackTarget = [&]() {
    const std::filesystem::path file = currentPlaybackFile();
    if (const std::optional<int> trackIndex = currentPlaybackTrackIndex()) {
      if (const std::optional<PlaybackTarget> trackTarget =
              playbackTrackTarget(file, *trackIndex)) {
        return *trackTarget;
      }
    }
    return playbackFileTarget(file);
  };

  auto startPlayback = [&](playback_route::Route route,
                           playback_queue::Source source) {
    return mediaCoordinator
        .startPlayback(std::move(route), std::move(source))
        .accepted();
  };
  auto transportPlayback = [&](playback_queue::Direction direction) {
    return mediaCoordinator.transport(direction).accepted();
  };
  auto openBrowserMediaTarget = [&](const PlaybackTarget& target) {
    playback_route::Route route = playback_route::resolveTarget(target);
    const std::filesystem::path& targetFile = playbackTargetFile(target);
    if (isSupportedImageExt(targetFile)) {
      return mediaCoordinator
          .startFiles(std::move(route),
                      imageFilesFromBrowserEntries(browser.entries))
          .accepted();
    }
    return startPlayback(std::move(route),
                         browser_playback_source::capture(browser.entries));
  };
  auto playBrowserEntry = [&](const BrowserEntry& entry) {
    const std::optional<PlaybackTarget> target =
        browser_playback_source::targetFor(entry);
    return target && openBrowserMediaTarget(*target);
  };
  auto playOpenFilesRequest = [&](const OpenFilesRequest& request) {
    return mediaCoordinator.openFiles(request).accepted();
  };

  if (initialOpenRequest) {
    if (playOpenFilesRequest(*initialOpenRequest)) {
      markDirty(UiDirtyFlags::Async);
    }
  }

  struct CommandEntry {
    std::string label;
    std::string hotkey;
    bool enabled = true;
    std::function<void()> run;
  };

  struct FileContextMenuState {
    bool active = false;
    std::optional<BrowserEntry> entry;
    std::vector<playback_media_actions::Item> items;
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

  struct ActionRenderItem {
    ActionStripItem id;
    std::string label;
    std::string labelHover;
    bool active;
    int width;
  };

  auto selectedOptionsSubject = [&]()
      -> std::optional<OptionsBrowserSubject> {
    if (optionsBrowserIsActive(browser) || browser.entries.empty() ||
        browser.selected < 0 ||
        browser.selected >= static_cast<int>(browser.entries.size())) {
      return std::nullopt;
    }
    return optionsBrowserSubjectForEntry(
        browser.entries[static_cast<size_t>(browser.selected)]);
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
            case playback_overlay::OverlayControlId::EditMarkIn:
            case playback_overlay::OverlayControlId::EditMarkOut:
            case playback_overlay::OverlayControlId::EditClearSelection:
            case playback_overlay::OverlayControlId::EditRippleDelete:
            case playback_overlay::OverlayControlId::EditTrim:
            case playback_overlay::OverlayControlId::EditSuggestions:
            case playback_overlay::OverlayControlId::EditSuggestionFilter:
            case playback_overlay::OverlayControlId::EditPreviousSuggestion:
            case playback_overlay::OverlayControlId::EditNextSuggestion:
            case playback_overlay::OverlayControlId::EditSelectSuggestion:
            case playback_overlay::OverlayControlId::EditHideSuggestion:
            case playback_overlay::OverlayControlId::EditUndoHideSuggestion:
            case playback_overlay::OverlayControlId::EditDone:
            case playback_overlay::OverlayControlId::EditStartExport:
            case playback_overlay::OverlayControlId::EditWaitForExport:
            case playback_overlay::OverlayControlId::EditCancelExport:
            case playback_overlay::OverlayControlId::EditConfirmPrompt:
            case playback_overlay::OverlayControlId::EditCancelPrompt:
            case playback_overlay::OverlayControlId::EditDiscardAndExit:
            case playback_overlay::OverlayControlId::EditCancelExit:
              return std::nullopt;
          }
          return std::nullopt;
        };

    playback_overlay::PlaybackOverlayState actionOverlayState;
    const std::filesystem::path nowPlaying = currentPlaybackFile();
    const std::optional<PlaybackControlState> videoControlState =
        mediaCoordinator.videoControlState();
    const bool videoActive = videoControlState.has_value();
    actionOverlayState.audioOk = videoActive || audioIsReady();
    actionOverlayState.playPauseAvailable = actionOverlayState.audioOk;
    actionOverlayState.audioSupports50HzToggle =
        audioIsReady() && audioSupports50HzToggle();
    actionOverlayState.canPlayPrevious =
        videoActive ? videoControlState->canPrevious
                    : actionOverlayState.audioOk || !nowPlaying.empty();
    actionOverlayState.canPlayNext =
        videoActive ? videoControlState->canNext
                    : actionOverlayState.audioOk || !nowPlaying.empty();
    actionOverlayState.radioEnabled = audioIsRadioEnabled();
    actionOverlayState.radioLabel = std::string(audioGetRadioFilterLabel());
    actionOverlayState.hz50Enabled = audioIs50HzEnabled();
    actionOverlayState.paused =
        videoActive
            ? videoControlState->status != PlaybackControlStatus::Playing
            : audioIsPaused() || audioIsFinished();
    actionOverlayState.pictureInPictureAvailable =
        videoActive || audioPictureInPicture.isOpen() ||
        actionOverlayState.audioOk || !nowPlaying.empty();
    const std::optional<PlaybackPresentationState> videoPresentation =
        mediaCoordinator.videoPresentationState();
    actionOverlayState.pictureInPictureActive =
        videoPresentation
            ? videoPresentation->layer() ==
                  PlaybackPresentationLayer::PictureInPicture
            : audioPictureInPicture.isOpen();
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
    const bool selectedEntryHasOptions =
        browserInteractionEnabled && selectedOptionsSubject().has_value();
    if (browserInteractionEnabled &&
        (optionsBrowserIsActive(browser) || selectedEntryHasOptions)) {
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
    const std::optional<media_processing::TaskCompletion> completion =
        mediaTasks.latestCompletion();
    const bool hasMediaTaskStatus =
        completion && !mediaTaskStatusModel(*completion).text.empty();
    const std::filesystem::path nowPlaying = currentPlaybackFile();
    const bool showNowPlaying =
        !nowPlaying.empty() || audioIsReady() || audioIsSeeking() ||
        audioIsHolding();
    BrowserFooterLayoutInput layoutInput;
    layoutInput.browserInteractionEnabled = !melodyVisualizationEnabled;
    layoutInput.showWarning =
        !mediaCommandError.empty() || !audioGetWarning().empty();
    layoutInput.showMediaTaskStatus = hasMediaTaskStatus;
    layoutInput.enableTransportUi = o.play;
    layoutInput.showNowPlaying = showNowPlaying;
    layoutInput.showPeakMeter = o.play && audioIsReady();
    BrowserFooterLayout layout = computeBrowserFooterLayout(layoutInput);
    if (layout.showNowPlaying) {
      const int nowPlayingLines = std::max(
          1, wrappedLineCount(" " + buildNowPlayingLabel(),
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
    applyBrowserViewportRestore(browser, layout);
    breadcrumbLine = buildBreadcrumbLine(browser.location, width);
    if (!browserInteractionEnabled) {
      breadcrumbHover = -1;
    } else if (breadcrumbHover >= static_cast<int>(breadcrumbLine.crumbs.size())) {
      breadcrumbHover = -1;
    }
    layoutDirty = false;
  };

  InputCallbacks callbacks;
  callbacks.onQuit = [&]() { mediaCoordinator.requestQuit(); };
  callbacks.onActivateEntry = [&](const BrowserEntry& entry) {
    OptionsBrowserResult optionsResult =
        optionsBrowserActivateEntry(browser, entry);
    if (optionsResult == OptionsBrowserResult::Changed) {
      browserNavigator.reload();
      return true;
    }
    if (optionsResult == OptionsBrowserResult::Handled) {
      return true;
    }
    return playBrowserEntry(entry);
  };
  callbacks.onPlayFiles =
      [&](const std::vector<std::filesystem::path>& files) {
        return playOpenFilesRequest({files});
      };
  callbacks.onOpenFileContextMenu = [&](const BrowserEntry& entry, int x,
                                        int y) {
    if (!o.play || !entry.isMedia()) {
      return;
    }
    playback_media_actions::Context context;
    if (isVideoExt(entry.path)) {
      context.mediaKind = playback_media_actions::MediaKind::Video;
    } else if (isSupportedAudioExt(entry.path)) {
      context.mediaKind = playback_media_actions::MediaKind::Audio;
    }
    const bool audio =
        context.mediaKind == playback_media_actions::MediaKind::Audio;
    context.canBrowseTracks =
        audio && supportsPlaybackTrackCatalog(entry.path);
    context.canAnalyzeAudio =
        audio && audioCanAnalyzeFileToMelodyFile(entry.path);
    context.hasGeneratedSubtitles =
        !playback_video_transcript::activeTranscriptPathForVideo(entry.path)
             .empty();
    mediaProcessingActions.applySourceState(entry.path, &context);
    std::vector<playback_media_actions::Item> items =
        playback_media_actions::build(context);
    if (items.empty()) return;
    fileContextMenu.active = true;
    fileContextMenu.entry = entry;
    fileContextMenu.items = std::move(items);
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
    if (mediaCoordinator.videoActive()) {
      mediaCoordinator.handleControlCommand(PlaybackControlCommand::Play);
    } else {
      audioPlay();
    }
    markDirty();
  };
  callbacks.onPause = [&]() {
    if (mediaCoordinator.videoActive()) {
      mediaCoordinator.handleControlCommand(PlaybackControlCommand::Pause);
    } else {
      audioPause();
    }
    markDirty();
  };
  callbacks.onTogglePause = [&]() {
    if (mediaCoordinator.videoActive()) {
      mediaCoordinator.handleControlCommand(
          PlaybackControlCommand::TogglePause);
    } else {
      audioTogglePause();
    }
    markDirty();
  };
  callbacks.onStopPlayback = [&]() {
    if (mediaCoordinator.videoActive()) {
      mediaCoordinator.stopVideo();
      markDirty();
    } else if (audioIsReady()) {
      audioStop();
      markDirty();
    }
  };
  callbacks.onCurrentPlaybackFile = [&]() { return currentPlaybackFile(); };
  callbacks.onPlayPrevious = [&]() {
    if (currentPlaybackFile().empty()) {
      return;
    }
    if (transportPlayback(playback_queue::Direction::Previous)) {
      markDirty();
    }
  };
  callbacks.onPlayNext = [&]() {
    if (currentPlaybackFile().empty()) {
      return;
    }
    if (transportPlayback(playback_queue::Direction::Next)) {
      markDirty();
    }
  };
  callbacks.onToggleRadio = [&]() {
    audioCycleRadioFilter();
    markDirty();
  };
  callbacks.onToggle50Hz = [&]() {
    if (audioSupports50HzToggle()) {
      audioToggle50Hz();
      markDirty();
    }
  };
  callbacks.onToggleOptions = [&]() {
    if (optionsBrowserIsActive(browser)) {
      browserNavigator.closeContext();
      return;
    }
    if (const auto subject = selectedOptionsSubject()) {
      browserNavigator.navigate(optionsBrowserOpenLocation(*subject));
    }
  };
  callbacks.onSeekBy = [&](int direction) {
    audioSeekBy(direction);
    markDirty();
  };
  callbacks.onSeekToRatio = [&](double ratio) {
    if (mediaCoordinator.videoActive()) {
      mediaCoordinator.seekVideoToRatio(ratio);
    } else {
      audioSeekToRatio(ratio);
    }
    markDirty();
  };
  callbacks.onAdjustVolume = [&](float delta) {
    audioAdjustVolume(delta);
    markDirty();
  };
  callbacks.onToggleWindow = [&]() {
    if (mediaCoordinator.videoActive()) {
      if (mediaCoordinator.toggleWindowPresentation()) markLayoutDirty();
      return;
    }
    if (!audioPictureInPicture.isOpen() && audioGetNowPlaying().empty() &&
        !audioIsReady()) {
      return;
    }
    if (audioPictureInPicture.toggle()) {
      markDirty(UiDirtyFlags::Async);
      return;
    }
    showAudioPictureInPictureOpenError();
    markDirty();
  };
  callbacks.onTogglePictureInPicture = [&]() {
    if (mediaCoordinator.videoActive()) {
      if (mediaCoordinator.togglePictureInPicture()) markLayoutDirty();
      return;
    }
    if (callbacks.onToggleWindow) callbacks.onToggleWindow();
  };
  callbacks.onToggleFullscreen = [&]() {
    if (mediaCoordinator.toggleFullscreen()) markLayoutDirty();
  };
  callbacks.onPlaybackContextShortcut = [&](PlaybackShortcutAction action) {
    switch (action) {
      case PlaybackShortcutAction::DismissPictureInPicture:
        if (callbacks.onTogglePictureInPicture) {
          callbacks.onTogglePictureInPicture();
        }
        break;
      default:
        break;
    }
  };

  AudioPictureInPictureWindow::Styles audioPictureInPictureStyles{kStyleNormal,
                                          kStyleAccent,
                                          kStyleDim,
                                          kStyleAlert,
                                          kStyleActionActive,
                                          kStyleProgressEmpty,
                                          kStyleProgressFrame,
                                          kProgressStart,
                                          kProgressEnd};
  auto buildAudioPictureInPictureContext = [&]() {
    AudioPictureInPictureWindow::Context context;
    context.nowPlayingLabel = buildNowPlayingLabel();
    const std::filesystem::path nowPlaying = audioGetNowPlaying();
    if (!nowPlaying.empty()) {
      context.nowPlayingTarget = playbackFileTarget(nowPlaying);
      if (const std::optional<PlaybackTarget> trackTarget =
              playbackTrackTarget(nowPlaying, audioGetTrackIndex())) {
        context.nowPlayingTarget = *trackTarget;
      }
    }
    return context;
  };
  auto renderAudioPictureInPicture = [&]() {
    if (!audioPictureInPicture.isOpen()) {
      return;
    }
    audioPictureInPicture.render(audioPictureInPictureStyles,
                                 buildAudioPictureInPictureContext());
  };
  AudioPictureInPictureWindow::Callbacks audioPictureInPictureCallbacks;
  audioPictureInPictureCallbacks.onQuit = [&]() {
    if (callbacks.onQuit) callbacks.onQuit();
  };
  audioPictureInPictureCallbacks.onTogglePause = [&]() {
    if (callbacks.onTogglePause) callbacks.onTogglePause();
  };
  audioPictureInPictureCallbacks.onStopPlayback = [&]() {
    if (callbacks.onStopPlayback) callbacks.onStopPlayback();
  };
  audioPictureInPictureCallbacks.onPlayPrevious = [&]() {
    if (callbacks.onPlayPrevious) callbacks.onPlayPrevious();
  };
  audioPictureInPictureCallbacks.onPlayNext = [&]() {
    if (callbacks.onPlayNext) callbacks.onPlayNext();
  };
  audioPictureInPictureCallbacks.onToggleRadio = [&]() {
    if (callbacks.onToggleRadio) callbacks.onToggleRadio();
  };
  audioPictureInPictureCallbacks.onToggle50Hz = [&]() {
    if (callbacks.onToggle50Hz) callbacks.onToggle50Hz();
  };
  audioPictureInPictureCallbacks.onSeekBy = [&](int direction) {
    if (callbacks.onSeekBy) callbacks.onSeekBy(direction);
  };
  audioPictureInPictureCallbacks.onSeekToRatio = [&](double ratio) {
    if (callbacks.onSeekToRatio) callbacks.onSeekToRatio(ratio);
  };
  audioPictureInPictureCallbacks.onAdjustVolume = [&](float delta) {
    if (callbacks.onAdjustVolume) callbacks.onAdjustVolume(delta);
  };
  audioPictureInPictureCallbacks.onPlayFiles =
      [&](const std::vector<std::filesystem::path>& files) {
    WindowPlacementState sourcePlacement =
        audioPictureInPicture.capturePlacement();
    PlaybackPresentationState videoPresentation =
        videoConfig.enableAscii
            ? PlaybackPresentationState::terminalAscii()
            : PlaybackPresentationState::nativeWindowed();
    videoPresentation = videoPresentation.togglePictureInPicture();
    return mediaCoordinator
        .startDroppedFiles(files, &sourcePlacement, videoPresentation)
        .accepted();
  };
  audioPictureInPictureCallbacks.onClose =
      [&]() { markDirty(UiDirtyFlags::Async); };

  auto handleSystemPlaybackCommand = [&](PlaybackControlCommand command) {
    if (mediaCoordinator.videoActive()) {
      mediaCoordinator.handleControlCommand(command);
      markDirty();
      return;
    }
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

  auto activateRadioifySurface = [&]() {
    if (mediaCoordinator.videoActive()) {
      mediaCoordinator.activateVideoPresentation();
      return;
    }
    if (windowTuiEnabled && tuiWindow.IsOpen()) {
      tuiWindow.Activate();
      return;
    }
    if (audioPictureInPicture.isOpen()) {
      audioPictureInPicture.activate();
      return;
    }
    activateWindowsConsoleWindow();
  };

  auto buildAudioControlState = [&]()
      -> std::optional<PlaybackControlState> {
    std::filesystem::path nowPlaying = audioGetNowPlaying();
    if (nowPlaying.empty()) {
      return std::nullopt;
    }

    PlaybackTarget target = playbackFileTarget(nowPlaying);
    if (const std::optional<PlaybackTarget> trackTarget =
            playbackTrackTarget(nowPlaying, audioGetTrackIndex())) {
      target = *trackTarget;
    }
    PlaybackControlState state(std::move(target), false);
    state.positionSec = audioGetTimeSec();
    const double durationSec = audioGetTotalSec();
    if (std::isfinite(durationSec) && durationSec > 0.0) {
      state.durationSec = durationSec;
    }
    state.canPlay = true;
    state.canPause = true;
    state.canStop = true;
    state.canPrevious = true;
    state.canNext = true;
    if (audioIsFinished()) {
      state.status = PlaybackControlStatus::Stopped;
    } else if (audioIsPaused()) {
      state.status = PlaybackControlStatus::Paused;
    } else {
      state.status = PlaybackControlStatus::Playing;
    }
    return state;
  };

  auto syncShellControls = [&]() {
    std::optional<PlaybackControlState> state =
        mediaCoordinator.videoControlState();
    if (!state) {
      state = buildAudioControlState();
    }
    if (!state) {
      systemControls.clear();
      notificationAreaControls.clear();
      return;
    }
    systemControls.update(*state);
    notificationAreaControls.update(*state);
  };

  auto handleNotificationAreaCommand =
      [&](const PlaybackNotificationAreaCommand& command) {
    switch (command.kind) {
      case PlaybackNotificationAreaCommand::Kind::Activate:
        activateRadioifySurface();
        break;
      case PlaybackNotificationAreaCommand::Kind::Playback:
        handleSystemPlaybackCommand(command.playbackCommand);
        break;
      case PlaybackNotificationAreaCommand::Kind::Quit:
        if (callbacks.onQuit) callbacks.onQuit();
        break;
    }
  };

  auto processShellPlaybackCommands = [&]() {
    PlaybackControlCommand command;
    while (systemControls.pollCommand(&command)) {
      handleSystemPlaybackCommand(command);
    }
    PlaybackNotificationAreaCommand notificationCommand;
    while (notificationAreaControls.pollCommand(&notificationCommand)) {
      handleNotificationAreaCommand(notificationCommand);
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
                      if (callbacks.onTogglePause) {
                        callbacks.onTogglePause();
                      }
                    }});
    if (mediaCoordinator.videoActive()) {
      cmds.push_back({"Window mode (framebuffer)", "Ctrl+W", true, [&]() {
                        if (callbacks.onToggleWindow) callbacks.onToggleWindow();
                      }});
      cmds.push_back({"Fullscreen", "Alt+Enter", true, [&]() {
                        if (callbacks.onToggleFullscreen) {
                          callbacks.onToggleFullscreen();
                        }
                      }});
    }
    if (mediaCoordinator.videoActive() || audioPictureInPicture.isOpen() ||
        !audioGetNowPlaying().empty() || audioIsReady()) {
      cmds.push_back({"Picture-in-Picture", "Ctrl+P", true, [&]() {
                        if (callbacks.onTogglePictureInPicture) {
                          callbacks.onTogglePictureInPicture();
                        }
                      }});
    }
    cmds.push_back({"Cycle Radio Filter",
                    "R", true, [&]() {
                      audioCycleRadioFilter();
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
      const bool selectedEntryHasOptions =
          selectedOptionsSubject().has_value();
      if (optionsBrowserIsActive(browser) || selectedEntryHasOptions) {
        cmds.push_back({"Options", "O", true, [&]() {
                          if (callbacks.onToggleOptions) {
                            callbacks.onToggleOptions();
                          }
                        }});
      }
      if (!currentPlaybackFile().empty()) {
        cmds.push_back({"Show Playing File", "", true, [&]() {
                          browserPlaybackRevealer.reveal(
                              currentPlaybackTarget());
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

  auto buildMelodyOutputPath = [&](const BrowserEntry& entry) {
    std::filesystem::path output = entry.path;
    if (const auto* track = entry.actionAs<browser_entry::PlayTrack>()) {
      char suffix[32];
      std::snprintf(suffix, sizeof(suffix), ".track%03d.melody",
                    track->trackIndex);
      output += suffix;
      return output;
    }
    output.replace_extension(".melody");
    return output;
  };

  auto startMelodyExport = [&](const BrowserEntry& entry) {
    if (!entry.isMedia() || !isSupportedAudioExt(entry.path)) {
      return;
    }

    const auto* track = entry.actionAs<browser_entry::PlayTrack>();
    const int trackIndex = track ? track->trackIndex : 0;
    if (mediaTasks.tryStartMelodyAnalysis(
            entry.path, trackIndex, buildMelodyOutputPath(entry))) {
      markLayoutDirty();
      markDirty(UiDirtyFlags::Async);
    }
  };

  auto computeFileContextLayout = [&](int w, int h, int topInset) {
    FileContextMenuLayout layout{};
    if (fileContextMenu.items.empty()) return layout;
    int itemWidth = 0;
    for (const auto& item : fileContextMenu.items) {
      itemWidth = std::max(itemWidth, utf8DisplayWidth(item.label));
    }
    layout.width = std::max(18, itemWidth + 4);
    layout.height = static_cast<int>(fileContextMenu.items.size()) + 2;
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
    layout.rows = static_cast<int>(fileContextMenu.items.size());
    layout.valid = true;
    return layout;
  };

  auto runFileContextAction = [&](int actionIndex) {
    if (!fileContextMenu.active || !fileContextMenu.entry || actionIndex < 0 ||
        actionIndex >= static_cast<int>(fileContextMenu.items.size())) {
      return;
    }
    const BrowserEntry entry = *fileContextMenu.entry;
    const playback_media_actions::Action action =
        fileContextMenu.items[static_cast<size_t>(actionIndex)].action;
    fileContextMenu.active = false;
    fileContextMenu.entry.reset();
    fileContextMenu.items.clear();
    dirty = true;
    if (action == playback_media_actions::Action::Play) {
      if (playBrowserEntry(entry)) {
        markDirty(UiDirtyFlags::Async);
      }
    } else if (action == playback_media_actions::Action::BrowseTracks) {
      browserNavigator.navigate(
          browserTrackLocation(normalizeTrackBrowserPath(entry.path)));
    } else if (action == playback_media_actions::Action::EditVideo) {
      playback_route::Route route =
          playback_route::resolveTarget(playbackFileTarget(entry.path));
      route.sessionIntent = PlaybackSessionIntent::EditVideo;
      const PlaybackTarget target = route.target;
      if (startPlayback(std::move(route),
                        playback_queue::singleSource(target))) {
        markDirty(UiDirtyFlags::Async);
      }
    } else if (action ==
               playback_media_actions::Action::GenerateSubtitles) {
      mediaProcessingActions.requestSubtitles(entry.path);
    } else if (action ==
               playback_media_actions::Action::CancelSubtitleGeneration) {
      mediaProcessingActions.requestSubtitleCancellation();
    } else if (action == playback_media_actions::Action::SeparateAudio) {
      mediaProcessingActions.requestAudioSeparation(entry.path);
    } else if (action ==
               playback_media_actions::Action::CancelAudioSeparation) {
      mediaProcessingActions.requestAudioSeparationCancellation();
    } else if (action == playback_media_actions::Action::AnalyzeAudio) {
      startMelodyExport(entry);
    } else if (action == playback_media_actions::Action::SplitLoop) {
      if (entry.isMedia() && isSupportedAudioExt(entry.path)) {
        LoopSplitConfig splitConfig;
        splitConfig.channels = 2;
        splitConfig.sampleRate = 48000;
        const auto* track = entry.actionAs<browser_entry::PlayTrack>();
        splitConfig.trackIndex = track ? track->trackIndex : 0;
        splitConfig.kssOptions = audioGetKssOptionState();
        splitConfig.nsfOptions = audioGetNsfOptionState();
        splitConfig.vgmOptions = audioGetVgmOptionState();
        const auto outputPaths =
            resolveSplitOutputPaths(entry.path, o.output);
        if (mediaTasks.tryStartLoopSplit(
                entry.path, outputPaths.first, outputPaths.second,
                splitConfig)) {
          markLayoutDirty();
          markDirty(UiDirtyFlags::Async);
        }
      }
    }
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

  PlaybackShellTerminalRole previousTerminalRole =
      mediaCoordinator.terminalRole();
  while (running) {
    if (mediaCoordinator.pump()) {
      markDirty(UiDirtyFlags::Async);
    }
    if (!running) break;
    syncShellControls();
    const PlaybackShellTerminalRole terminalRole =
        mediaCoordinator.terminalRole();
    if (terminalRole != previousTerminalRole) {
      previousTerminalRole = terminalRole;
      browserDoubleClickTracker.reset();
      markLayoutDirty();
      forceFullRedraw = true;
    }
    while (std::optional<BrowserContentWorker::Completion>
               browserContentCompletion = browserContentWorker.poll()) {
      const bool initialCompletion =
          initialBrowserPreparationId &&
          *initialBrowserPreparationId == browserContentCompletion->generation;
      const bool initialPreparationFailed =
          initialCompletion &&
          (!browserContentCompletion->result ||
           std::holds_alternative<BrowserPreparationError>(
               *browserContentCompletion->result));
      BrowserPreparationResult result =
          browserContentCompletion->result
              ? std::move(*browserContentCompletion->result)
              : BrowserPreparationResult(BrowserPreparationError{
                    BrowserPreparationErrorKind::Internal, browser.location,
                    "The browser preparation worker stopped unexpectedly."});
      browserNavigator.completePreparation(browserContentCompletion->generation,
                                           std::move(result));
      if (initialCompletion) {
        initialBrowserPreparationId.reset();
      }
      if (initialPreparationFailed && !startDir.empty()) {
        browserNavigator.initialize(browserDirectoryLocation({}));
      }
    }
    if (browserSelectionMetadata.poll()) {
      markDirty(UiDirtyFlags::Async);
    }
    if (layoutDirty) {
      rebuildLayout();
    }
    const media_processing::PollResult mediaTaskUpdate = mediaTasks.poll();
    for (const media_processing::TaskCompletion& completion :
         mediaTaskUpdate.completions) {
      const MediaTaskStatusModel status = mediaTaskStatusModel(completion);
      if (completion.kind ==
          media_processing::TaskKind::SubtitleGeneration) {
        mediaCoordinator.subtitleGenerationFinishedFor(
            completion.sourceFile, completion.outputFile,
            completion.succeeded(), status.text);
      } else if (completion.kind ==
                 media_processing::TaskKind::AudioSeparation) {
        mediaCoordinator.mediaTaskFinishedFor(completion.sourceFile,
                                              status.text);
      }
      markLayoutDirty();
    }
    if (mediaTaskUpdate.changed) markDirty(UiDirtyFlags::Async);

    if (windowTuiEnabled && tuiWindow.IsOpen()) {
      tuiWindow.PollEvents();
    }
    if (audioPictureInPicture.isOpen() &&
        audioPictureInPicture.pollEvents(audioPictureInPictureCallbacks)) {
      markDirty(UiDirtyFlags::Async);
    }
    processShellPlaybackCommands();
    if (!running) break;

    if (consumeBrowserThumbnailWake()) {
      markDirty(UiDirtyFlags::Async);
    }

    OpenFilesRequest openRequest;
    while (openFileRequests.poll(openRequest)) {
      if (playOpenFilesRequest(openRequest)) {
        markDirty(UiDirtyFlags::Async);
      }
    }

    if (mediaCoordinator.terminalRole() !=
        PlaybackShellTerminalRole::Browser) {
      InputEvent playbackEvent{};
      if (consoleInputPump.pollNext(input, playbackEvent)) {
        if (playbackEvent.type == InputEvent::Type::Resize) {
          screen.updateSize();
        }
        if (playbackEvent.type == InputEvent::Type::Key &&
            playbackEvent.key.vk == VK_F8 &&
            cancelActiveMediaTask()) {
          continue;
        }
        mediaCoordinator.handleVideoInputEvent(playbackEvent);
        continue;
      }
      dirty = false;
      dirtyFlags = UiDirtyFlags::None;
      forceFullRedraw = false;
      const std::vector<NativeWaitHandle> playbackActivityHandles =
          mediaActivityWaitHandles();
      const int playbackTimeoutMs =
          std::max(0, mediaCoordinator.nextWakeTimeoutMs());
      waitForBrowserWake(
          input, openFileRequests, browserThumbnailWakeHandle(),
          browserContentWorker.nativeWaitHandle(),
          browserSelectionMetadata.nativeWaitHandle(),
          notificationAreaControls.nativeWaitHandle(), tuiWindow,
          audioPictureInPicture, playbackActivityHandles,
          static_cast<DWORD>(playbackTimeoutMs));
      continue;
    }

    auto processInputEvent = [&](InputEvent ev) {
      if (ev.type == InputEvent::Type::Mouse) {
        browserDoubleClickTracker.classifyUsingSystemSettings(
            ev.mouse, screen.cellPixelWidth(), screen.cellPixelHeight());
      } else {
        browserDoubleClickTracker.reset();
      }
      if (ev.type == InputEvent::Type::Resize) {
        dirty = true;
        if (callbacks.onResize) callbacks.onResize();
        return;
      }
      if (ev.type == InputEvent::Type::Key && ev.key.vk == VK_F8 &&
          cancelActiveMediaTask()) {
        return;
      }
      if (mediaCoordinator.capturesBrowserInput()) {
        if (ev.type == InputEvent::Type::Key ||
            ev.type == InputEvent::Type::Action) {
          mediaCoordinator.handleVideoInputEvent(ev);
          markDirty(UiDirtyFlags::Async);
        }
        return;
      }
      if (ev.type == InputEvent::Type::FileDrop &&
          isCommittedFileDropEvent(ev.fileDrop) && callbacks.onPlayFiles &&
          callbacks.onPlayFiles(ev.fileDrop.files)) {
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
                         isMouseButtonDown(ev.mouse, MouseButton::Left);
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
        // Open command palette with F1 (no modifier). Previously Ctrl+P
        // conflicted with the playback Ctrl+P PiP binding.
        bool paletteToggle = (key.vk == VK_F1);
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
        browserNavigator.reload();
        markDirty();
        return;
      }
      if (ev.type == InputEvent::Type::Action &&
          ev.action == InputAction::Back) {
        if (fileContextMenu.active) {
          fileContextMenu.active = false;
          dirty = true;
          return;
        }
        if (paletteActive) {
          paletteActive = false;
          dirty = true;
          return;
        }
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
          bool leftPressed = isMouseButtonDown(mouse, MouseButton::Left);
          if (mouse.kind == MouseEventKind::VerticalWheel) {
            int delta = mouse.wheelDelta;
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
          if (mouse.kind == MouseEventKind::Move) {
            if (fileContextLayout.valid &&
                mouse.pos.X >= fileContextLayout.x &&
                mouse.pos.X < fileContextLayout.x + fileContextLayout.width &&
                mouse.pos.Y >= fileContextLayout.listY &&
                mouse.pos.Y < fileContextLayout.listY + fileContextLayout.rows) {
              int action = mouse.pos.Y - fileContextLayout.listY;
              if (action >= 0 && action < fileContextLayout.rows &&
                  fileContextMenu.selected != action) {
                fileContextMenu.selected = action;
                dirty = true;
              }
            }
            return;
          }
          if (mouse.kind == MouseEventKind::Press && leftPressed) {
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
          bool leftPressed = isMouseButtonDown(mouse, MouseButton::Left);
          if (mouse.kind == MouseEventKind::VerticalWheel) {
            int delta = mouse.wheelDelta;
            if (delta != 0) {
              paletteSelected -= delta / WHEEL_DELTA;
              ensurePaletteScroll(static_cast<int>(paletteFiltered.size()),
                                  visibleRows);
              dirty = true;
            }
            return;
          }
          if (leftPressed && mouse.kind == MouseEventKind::Press) {
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
      if (mediaCoordinator.videoActive() &&
          !browser.filterActive && !browser.pathSearchActive &&
          (ev.type == InputEvent::Type::Key ||
           ev.type == InputEvent::Type::Action)) {
        const std::optional<PlaybackShortcutAction> action =
            resolveLiveBrowserVideoShortcut(ev);
        if (action && mediaCoordinator.handleVideoInputEvent(ev)) {
          markDirty(UiDirtyFlags::Async);
          return;
        }
      }
      handleInputEvent(
          ev, browserNavigator, browserEntryClickTracker, browserPointerState,
          layout,
          breadcrumbLine, breadcrumbY, searchBarY, searchBarWidth, listTop,
          listHeight, progressBarX, progressBarY, progressBarWidth, actionStrip,
          browserInteractionEnabled, o.play, audioIsReady(), breadcrumbHover,
          actionHover, searchBarHover, dirty, running, callbacks);
    };

    auto finalizeRenderedExit = [&]() {
      audioPictureInPicture.close();
      if (windowTuiEnabled && tuiWindow.IsOpen()) {
        tuiWindow.Close();
      }
      mediaTasks.shutdown();
      audioShutdown();
    };

    auto flushLayoutIfNeeded = [&]() {
      if (layoutDirty && hasDirtyFlag(dirtyFlags, UiDirtyFlags::Layout)) {
        rebuildLayout();
      }
    };

    auto dispatchInputEvent = [&](const InputEvent& event) -> bool {
      processShellPlaybackCommands();
      if (!running) return true;
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
      while (running &&
             mediaCoordinator.terminalRole() ==
                 PlaybackShellTerminalRole::Browser &&
             tuiWindow.PollInput(ev)) {
        if (!dispatchInputEvent(ev)) return 0;
        if (ev.type == InputEvent::Type::Resize) {
          flushLayoutIfNeeded();
        }
        if (!running) break;
      }
    }

    if (running &&
        mediaCoordinator.terminalRole() ==
            PlaybackShellTerminalRole::Browser &&
        consoleInputPump.pollNext(input, ev)) {
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
    if (mediaCoordinator.terminalRole() !=
        PlaybackShellTerminalRole::Browser) {
      continue;
    }

    processShellPlaybackCommands();
    if (!running) break;
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
          (audioIsReady() || !currentPlaybackFile().empty())) {
        reduceTimeout(std::chrono::milliseconds(100));
      }
      if (audioPictureInPicture.isOpen()) {
        reduceTimeout(std::chrono::milliseconds(100));
      }
      if (mediaCoordinator.videoActive()) {
        const DWORD playbackTimeout = static_cast<DWORD>(
            std::max(0, mediaCoordinator.nextWakeTimeoutMs()));
        timeout = timeout == INFINITE
                      ? playbackTimeout
                      : std::min(timeout, playbackTimeout);
      }
      return timeout;
    };

    if (!dirty) {
      DWORD waitTimeout = computeWakeTimeout(now);
      const std::vector<NativeWaitHandle> playbackActivityHandles =
          mediaActivityWaitHandles();
      DWORD waitResult = waitForBrowserWake(
          input, openFileRequests, browserThumbnailWakeHandle(),
          browserContentWorker.nativeWaitHandle(),
          browserSelectionMetadata.nativeWaitHandle(),
          notificationAreaControls.nativeWaitHandle(), tuiWindow,
          audioPictureInPicture, playbackActivityHandles, waitTimeout);
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

      const std::string headerTitleRaw = RADIOIFY_APP_NAME;
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
      breadcrumbLine = buildBreadcrumbLine(browser.location, width);
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
      std::filesystem::path nowPlaying = currentPlaybackFile();
      const std::optional<int> nowPlayingTrackIndex =
          currentPlaybackTrackIndex();
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
        showingLabel = optionsBrowserShowingLabel(browser);
      } else if (trackMode) {
        showingLabel =
            "  Showing: tracks in " +
            toUtf8String(browser.location.path().filename());
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
        const int playingEntryIndex = findBrowserPlaybackTargetEntry(
            browser.entries, currentPlaybackTarget());
        drawBrowserEntries(screen, browser, layout, listTop, listHeight,
                           kStyleNormal, kStyleNormal, kStyleDir,
                           kStyleHighlight, kStyleBrowserHover, kStyleDim,
                           kStyleAccent,
                           playingEntryIndex, isSupportedImageExt, isVideoExt,
                           isSupportedAudioExt);
      } else {
        drawMelodyPanel(listTop, listHeight, width, melodyInfo,
                        melodyAnalysisState);
      }

      int footerStart = listTop + listHeight;
      int line = footerStart;
      if (line < height && footerLayout.showMeta) {
        std::string meta;
        Style metaStyle = kStyleDim;
        if (browser.contentLoading) {
          meta = " Loading...";
        } else if (!browser.contentError.empty()) {
          meta = " Error: " + browser.contentError;
          metaStyle = kStyleAlert;
        } else if (optionsMode) {
          meta = optionsBrowserSelectionMeta(browser);
        } else if (trackMode) {
          meta = buildTrackSelectionMeta(browser);
        } else {
          meta = browserSelectionMetadata.describe(browser);
        }
        if (!meta.empty()) {
          screen.writeText(0, line++, fitLine(meta, width), metaStyle);
        }
      }
      if (line < height && footerLayout.showWarning) {
        if (!mediaCommandError.empty()) {
          screen.writeText(
              0, line++, fitLine("  Error: " + mediaCommandError, width),
              kStyleAlert);
        } else {
          std::string warning = audioGetWarning();
          if (!warning.empty()) {
          screen.writeText(0, line++, fitLine("  Warning: " + warning, width),
                           kStyleDim);
          }
        }
      }
      if (line < height && footerLayout.showMediaTaskStatus) {
        const std::optional<media_processing::TaskCompletion> completion =
            mediaTasks.latestCompletion();
        if (completion) {
          const MediaTaskStatusModel status =
              mediaTaskStatusModel(*completion);
          if (!status.text.empty()) {
            screen.writeText(
                0, line++, fitLine(" " + status.text, width),
                status.succeeded ? kStyleDim : kStyleAlert);
          }
        }
      }
      std::string nowLabel = buildNowPlayingLabel();
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

      const std::optional<PlaybackControlState> videoControlState =
          mediaCoordinator.videoControlState();
      const bool videoActive = videoControlState.has_value();
      bool audioReady = audioIsReady();
      double currentSec = videoActive
                              ? videoControlState->positionSec
                              : (audioReady ? audioGetTimeSec() : 0.0);
      double totalSec = videoActive
                            ? videoControlState->durationSec.value_or(-1.0)
                            : (audioReady ? audioGetTotalSec() : -1.0);
      double displaySec = currentSec;
      if (!videoActive && audioReady && audioIsSeeking()) {
        double seekSec = audioGetSeekTargetSec();
        if (seekSec >= 0.0 && std::isfinite(seekSec)) {
          displaySec = seekSec;
        }
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
      footerInput.unclippedOutputPeak = audioGetUnclippedOutputPeak();
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

          int inner = std::max(1, w - 2);
          for (int i = 0; i < fileContextLayout.rows; ++i) {
            if (i < 0 ||
                i >= static_cast<int>(fileContextMenu.items.size())) {
              continue;
            }
            std::string text =
                " " + fileContextMenu.items[static_cast<size_t>(i)].label;
            if (utf8DisplayWidth(text) > inner) {
              text = utf8TakeDisplayWidth(text, inner);
            }
            int textWidth = utf8DisplayWidth(text);
            if (textWidth < inner) {
              text.append(static_cast<size_t>(inner - textWidth), ' ');
            }
            Style rowStyle = kStyleNormal;
            if (i == fileContextMenu.selected) {
              rowStyle = kStyleHighlight;
            }
            screen.writeText(x0 + 1, fileContextLayout.listY + i, text, rowStyle);
          }
        }
      } else {
        fileContextLayout.valid = false;
      }

      if (const std::optional<media_processing::TaskActivity> activity =
              mediaTasks.activity()) {
        drawMediaTaskCard(
            screen, width, height, listTop, mediaTaskCardModel(*activity),
            {kStyleNormal, kStyleAccent, kStyleDim, kStyleNormal});
      }

      screen.draw();
      screen.setAlwaysFullRedraw(false);
      if (windowTuiEnabled && tuiWindow.IsOpen()) {
        int gridW = 0;
        int gridH = 0;
        if (screen.snapshot(windowCells, gridW, gridH)) {
          tuiWindow.PresentTextGrid(windowCells, gridW, gridH);
        }
      }
      renderAudioPictureInPicture();
      lastDraw = now;
      dirty = false;
      dirtyFlags = UiDirtyFlags::None;
      forceFullRedraw = false;
    }
  }
  // Clear screen before exiting
  screen.clear(kStyleNormal);
  screen.draw();
  audioPictureInPicture.close();
  if (windowTuiEnabled && tuiWindow.IsOpen()) {
    tuiWindow.Close();
  }
  input.restore();
  screen.restore();
  std::cout << "\n";
  mediaTasks.shutdown();
  audioShutdown();
  return 0;
}
