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
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "app_common.h"
#include "app/application_runtime.h"
#include "app/media_processing_actions.h"
#include "app/media_processing_coordinator.h"
#include "app/playback_queue.h"
#include "app/playback_route.h"
#include "audio_picture_in_picture_window.h"
#include "audioplayback.h"
#include "browser_action_strip.h"
#include "browser_chrome.h"
#include "browser_wake_schedule.h"
#include "browser_media_menu.h"
#include "browser_playback_reveal.h"
#include "browser_playback_source.h"
#include "browser_thumbnail_cache.h"
#include "browser_content_service.h"
#include "browser_navigation.h"
#include "browser_model.h"
#include "browsermeta.h"
#include "consoleinput.h"
#include "consolescreen.h"
#include "core/open_file_requests.h"
#include "core/windows_app_resources.h"
#include "core/windows_message_pump.h"
#include "core/windows_console_window.h"
#include "core/windows_shell_open.h"
#include "media_coordinator.h"
#include "image_viewer.h"
#include "playback_presenter.h"
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
#include "playback/session/session.h"
#include "playback/system_media_transport/controls.h"
#include "playback_target_match.h"
#include "playback/target.h"
#include "mouse_double_click_tracker.h"
#include "tracklist.h"
#include "track_browser_state.h"
#include "tui_export.h"
#include "tui_theme.h"
#include "ui_helpers.h"
#include "ui_inputlogic.h"
#include "ui_input_pump.h"
#include "ui_viewport.h"
#include "media_task_card.h"
#include "media_task_presentation.h"
#include "melody_visualization.h"
#include "melody_visualization_renderer.h"
#include "playback/video/playback.h"
#include "playback/video/transcript/artifact.h"
#include "playback/video/framebuffer/window/window.h"
#include "windows_file_drop_apartment.h"
#include "media_formats.h"
#include "runtime_helpers.h"
#include "shell_command_catalog.h"
#include "shell_overlay_stack.h"
#include "shell_overlay_stack_renderer.h"
#include "shell_shortcuts.h"

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
                                    activityHandles,
                                wake_schedule::Deadline deadline) {
  std::vector<NativeWaitHandle> handles;
  handles.reserve(10 + activityHandles.size());
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
  for (NativeWaitHandle handle : activityHandles) append(handle);
  return waitForHandlesAndPumpThreadWindowMessages(
      static_cast<DWORD>(handles.size()),
      handles.empty() ? nullptr : handles.data(), deadline);
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

int runTui(Options o, ApplicationRuntime& runtime) {
  AudioPlaybackRuntime& audioPlayback = runtime.audioPlayback();
  GpuRuntime& gpu = runtime.gpu();
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
  std::optional<BrowserPreparationId> initialBrowserPreparationId;
  BrowserContentService browserContentService(
      {audioPlayback, sampleRate, o.mono ? 1u : 2u});
  BrowserNavigator browserNavigator(browser, browserContentService);
  BrowserSelectionMetadata browserSelectionMetadata(isVideoExt);
  BrowserThumbnailCache browserThumbnails;
  const bool initialBrowserPreparationAccepted =
      browserNavigator.initialize(browserDirectoryLocation(startDir),
                                  initialName);
  initialBrowserPreparationId = browserNavigator.pendingPreparationId();
  if (!initialBrowserPreparationAccepted) {
    initialBrowserPreparationId.reset();
    browserNavigator.initialize(browserDirectoryLocation({}));
  }
  input.init();

  ConsoleScreen screen;
  screen.init();
  input.enableTerminalMouseInput();

  VideoWindow tuiWindow(gpu);
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
    const RadioFilterMode activeMode = audioPlayback.radioFilterMode();
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
                 audioPlayback.radioEnabled());
    logLine("Done.");
  };

  const TuiTheme theme = radioifyTuiTheme();

  auto showPlaybackErrorDialog = [&](const std::filesystem::path& file) {
    std::string error = audioPlayback.warning();
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
        input, screen, theme.normal, theme.accent, theme.dim, title, message,
        detail, "Enter/Space/Esc: close");
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

  screen.clear(theme.normal);
  screen.draw();

  bool running = true;
  auto lastDraw = std::chrono::steady_clock::now();
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
  ActionStripLayout actionStrip;
  BrowserInteractionState browserInteraction;
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
  tui_melody_visualization::Model melodyVisualization;
  const tui_melody_visualization::Styles melodyVisualizationStyles =
      theme.pitchMonitorStyles();
  std::vector<ScreenCell> windowCells;
  AudioPictureInPictureWindow audioPictureInPicture(gpu);
  ConsoleInputPump consoleInputPump;
  pointer_input::MouseDoubleClickTracker browserDoubleClickTracker;
  BrowserViewport viewport;
  browser_chrome::Model browserChrome;
  auto showAudioPictureInPictureOpenError = [&]() {
    const std::string detail =
        audioPictureInPicture.lastError().empty()
            ? "The picture-in-picture window did not open."
            : audioPictureInPicture.lastError();
    playback_dialog::showInfoDialog(input, screen, theme.normal, theme.accent,
                                    theme.dim, "Picture-in-Picture Error",
                                    RADIOIFY_APP_NAME
                                    " could not open picture-in-picture.",
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

  BrowserPlaybackRevealer browserPlaybackRevealer(browserNavigator);
  auto consumeBrowserNavigationEvents = [&]() {
    const std::vector<BrowserNavigator::Event> events =
        browserNavigator.drainEvents();
    if (!events.empty()) markLayoutDirty();
  };

  std::string mediaCommandError;

  PlaybackSession::Dependencies mediaSessionDependencies{
      audioPlayback, gpu, input, screen, theme.playbackSessionAppearance()};
  playback_queue::Queue& playbackQueue = runtime.playbackQueue();
  media_processing::Coordinator& mediaProcessing = runtime.mediaProcessing();
  media_processing::Actions& mediaActions = runtime.mediaActions();
  playback_media_processing::Actions mediaProcessingActions =
      mediaActions.playbackActions();
  MediaTaskPresenter mediaTaskPresenter(mediaProcessing);
  TuiMediaCoordinator mediaCoordinator(
      {playbackQueue, mediaProcessingActions, mediaSessionDependencies,
       videoConfig});
  TuiPlaybackPresenter playbackPresenter(mediaCoordinator, audioPlayback);
  auto handleMediaCoordinatorEvent =
      [&](TuiMediaCoordinator::Event event) {
    std::visit(
        [&](auto&& value) {
          using Event = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<
                            Event,
                            TuiMediaCoordinator::ApplyAudioPictureInPicture>) {
            applyAudioPictureInPicturePlan(value.plan);
          } else if constexpr (
              std::is_same_v<Event,
                             TuiMediaCoordinator::CommandErrorChanged>) {
            mediaCommandError = value.message;
            markDirty(UiDirtyFlags::Async);
          } else if constexpr (
              std::is_same_v<Event,
                             TuiMediaCoordinator::AudioPlaybackFailed>) {
            showPlaybackErrorDialog(value.file);
          } else if constexpr (
              std::is_same_v<Event, TuiMediaCoordinator::ShowImages>) {
            applyAudioPictureInPicturePlan(value.audioPictureInPicture);
            image_viewer::Result result = image_viewer::run(
                std::move(value.sequence), input, screen, theme.normal,
                theme.accent, theme.dim, openFileRequests);
            if (result.exit == image_viewer::Exit::QuitRequested) {
              mediaCoordinator.requestQuit();
            } else if (result.openFiles) {
              mediaCoordinator.openFiles(*result.openFiles);
            }
            markDirty();
          } else if constexpr (
              std::is_same_v<Event, TuiMediaCoordinator::QuitRequested>) {
            running = false;
          } else if constexpr (
              std::is_same_v<Event,
                             TuiMediaCoordinator::PresentationFinished>) {
            markDirty();
          } else if constexpr (
              std::is_same_v<Event,
                             TuiMediaCoordinator::ActivateBrowserSurface>) {
            if (windowTuiEnabled && tuiWindow.IsOpen()) {
              tuiWindow.Activate();
            } else {
              activateWindowsConsoleWindow();
            }
          } else if constexpr (
              std::is_same_v<Event,
                             TuiMediaCoordinator::OpenBrowserDirectory>) {
            if (!browserNavigator.navigate(
                    browserDirectoryLocation(value.path))) {
              mediaCommandError = "Unable to open the requested folder.";
            } else {
              mediaCommandError.clear();
            }
            markDirty(UiDirtyFlags::Async);
          }
        },
        event);
  };
  auto mediaWaitHandles = [&]() {
    std::vector<NativeWaitHandle> handles = mediaCoordinator.waitHandles();
    if (NativeWaitHandle taskWake = mediaProcessing.waitHandle()) {
      handles.push_back(taskWake);
    }
    return handles;
  };
  auto cancelActiveMediaTask = [&]() {
    const bool accepted = mediaProcessing.cancelActive();
    if (accepted) {
      markLayoutDirty();
      markDirty(UiDirtyFlags::Async);
    }
    return accepted;
  };
  auto handleGlobalShellShortcut = [&](const InputEvent& event) {
    const auto action = tui_shell_shortcuts::resolve(
        event, tui_shell_shortcuts::context(
                   tui_shell_shortcuts::Context::Global));
    return action == tui_shell_shortcuts::Action::CancelMediaTask &&
           cancelActiveMediaTask();
  };
  auto buildPlaybackLabel =
      [&](const std::optional<PlaybackTarget>& target) {
    const std::filesystem::path nowPlaying =
        target ? playbackTargetFile(*target) : std::filesystem::path{};
    std::string label =
        nowPlaying.empty() ? std::string("(none)")
                           : toUtf8String(nowPlaying.filename());
    const std::optional<int> trackIndex =
        target ? playbackTargetTrackIndex(*target) : std::nullopt;
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
  auto startPlayback = [&](playback_route::Route route,
                           playback_queue::Source source) {
    return mediaCoordinator.startPlayback(std::move(route),
                                          std::move(source));
  };
  auto openBrowserMediaTarget = [&](const PlaybackTarget& target) {
    playback_route::Route route = playback_route::resolveTarget(target);
    const std::filesystem::path& targetFile = playbackTargetFile(target);
    if (isSupportedImageExt(targetFile)) {
      return mediaCoordinator.startFiles(
          std::move(route), imageFilesFromBrowserEntries(browser.entries));
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
    return mediaCoordinator.openFiles(request);
  };

  if (initialOpenRequest) {
    if (playOpenFilesRequest(*initialOpenRequest)) {
      markDirty(UiDirtyFlags::Async);
    }
  }

  shell_overlay_stack::Model shellOverlays;
  const shell_overlay_stack::Styles shellOverlayStyles{
      theme.popupMenuStyles(), theme.commandPaletteStyles()};

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

  auto buildBrowserChrome = [&]() {
    const PlaybackPresentationModel presentation = playbackPresenter.model();
    browser_chrome::Input chromeInput;
    chromeInput.audio = presentation.audio;
    chromeInput.playback = presentation.control;
    chromeInput.videoPresentation = presentation.videoPresentation;
    chromeInput.playbackTargetAvailable =
        presentation.currentTarget.has_value();
    chromeInput.audioPictureInPictureOpen =
        audioPictureInPicture.isOpen();
    chromeInput.melodyVisualizationActive = melodyVisualization.active();
    chromeInput.transportUiEnabled = o.play;
    chromeInput.optionsModeActive = optionsBrowserIsActive(browser);
    chromeInput.selectedEntryHasOptions =
        selectedOptionsSubject().has_value();
    const std::optional<MediaTaskStatusModel> mediaTaskStatus =
        mediaTaskPresenter.latestStatus();
    chromeInput.hasMediaTaskStatus =
        mediaTaskStatus && !mediaTaskStatus->text.empty();
    chromeInput.hasWarning =
        !mediaCommandError.empty() || !audioPlayback.warning().empty();
    chromeInput.viewMode = browser.viewMode;
    chromeInput.nowPlayingLabel =
        buildPlaybackLabel(presentation.currentTarget);
    chromeInput.width = screen.width();
    return browser_chrome::build(chromeInput);
  };

  auto rebuildLayout = [&]() {
    if (screenSizeDirty) {
      screen.updateSize();
      screenSizeDirty = false;
    }
    browserChrome = buildBrowserChrome();
    const bool browserInteractionEnabled = !melodyVisualization.active();
    const bool showHeaderLabel =
        browserInteractionEnabled &&
        (optionsBrowserIsActive(browser) || isTrackBrowserActive(browser));
    viewport = computeBrowserViewport(screen.width(), screen.height(),
                                      browserInteractionEnabled,
                                      showHeaderLabel,
                                      browserChrome.footer.reservedLines,
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
    listHeight =
        std::max(1, height - listTop - browserChrome.footer.reservedLines);
    layout = buildLayout(browser, width, listHeight);
    applyBrowserViewportRestore(browser, layout);
    breadcrumbLine = buildBreadcrumbLine(browser.location, width);
    if (!browserInteractionEnabled) {
      browserInteraction.breadcrumbHover = -1;
    } else if (browserInteraction.breadcrumbHover >=
               static_cast<int>(breadcrumbLine.crumbs.size())) {
      browserInteraction.breadcrumbHover = -1;
    }
    layoutDirty = false;
  };

  auto handlePlaybackControlCommand = [&](PlaybackControlCommand command) {
    if (mediaCoordinator.handleControlCommand(command)) {
      markDirty();
    }
  };
  auto activateBrowserEntry = [&](const BrowserEntry& entry) {
    OptionsBrowserResult optionsResult =
        optionsBrowserActivateEntry(browser, entry, audioPlayback);
    if (optionsResult == OptionsBrowserResult::Changed) {
      browserNavigator.reload();
      return true;
    }
    if (optionsResult == OptionsBrowserResult::Handled) {
      return true;
    }
    return playBrowserEntry(entry);
  };
  auto openFileContextMenu = [&](const BrowserEntry& entry, int x, int y) {
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
        audio && audioPlayback.canAnalyzeFile(entry.path);
    context.hasGeneratedSubtitles =
        !playback_video_transcript::activeTranscriptPathForVideo(entry.path)
             .empty();
    mediaActions.applySourceState(entry.path, context);
    std::vector<playback_media_actions::Item> items =
        playback_media_actions::build(context);
    if (items.empty()) return;
    tui_popup_menu::Anchor anchor;
    anchor.x = x;
    anchor.y = y;
    shellOverlays.openMediaMenu(entry, std::move(items), anchor);
    markDirty();
  };
  auto renderInputFile = [&](const std::filesystem::path& file) {
    renderFile(file);
    didRender = true;
  };
  auto toggleRadio = [&]() {
    audioPlayback.cycleRadioFilter();
    markDirty();
  };
  auto toggle50Hz = [&]() {
    if (audioPlayback.supports50HzToggle()) {
      audioPlayback.toggle50Hz();
      markDirty();
    }
  };
  auto togglePitchMonitor = [&]() {
    const PlaybackPresentationModel presentation = playbackPresenter.model();
    const AudioPlaybackSnapshot& audio = presentation.audio;
    if (!melodyVisualization.active() && !audio.source && !audio.ready) {
      return;
    }
    const bool active = melodyVisualization.toggle();
    if (active) {
      dirty = setBrowserSearchFocus(browser, BrowserSearchFocus::None) || dirty;
      browserInteraction.breadcrumbHover = -1;
      browserInteraction.actionHover = -1;
    }
    shellOverlays.dismiss();
    markLayoutDirty();
  };
  auto toggleOptions = [&]() {
    if (optionsBrowserIsActive(browser)) {
      browserNavigator.closeContext();
      return;
    }
    if (const auto subject = selectedOptionsSubject()) {
      browserNavigator.navigate(optionsBrowserOpenLocation(*subject));
    }
  };
  auto toggleWindowPresentation = [&]() {
    const PlaybackPresentationModel presentation = playbackPresenter.model();
    if (presentation.control && presentation.control->isVideo) {
      if (mediaCoordinator.toggleWindowPresentation()) markLayoutDirty();
      return;
    }
    const AudioPlaybackSnapshot& audio = presentation.audio;
    if (!audioPictureInPicture.isOpen() && !audio.source && !audio.ready) {
      return;
    }
    if (audioPictureInPicture.toggle()) {
      markDirty(UiDirtyFlags::Async);
      return;
    }
    showAudioPictureInPictureOpenError();
    markDirty();
  };
  auto togglePictureInPicture = [&]() {
    const PlaybackPresentationModel presentation = playbackPresenter.model();
    if (presentation.control && presentation.control->isVideo) {
      if (mediaCoordinator.togglePictureInPicture()) markLayoutDirty();
      return;
    }
    toggleWindowPresentation();
  };
  auto dispatchPlaybackShortcut = [&](PlaybackAction action) {
    switch (action) {
      case PlaybackAction::Quit:
        mediaCoordinator.requestQuit();
        break;
      case PlaybackAction::Play:
        handlePlaybackControlCommand(PlaybackControlCommand::Play);
        break;
      case PlaybackAction::Pause:
        handlePlaybackControlCommand(PlaybackControlCommand::Pause);
        break;
      case PlaybackAction::TogglePause:
        handlePlaybackControlCommand(PlaybackControlCommand::TogglePause);
        break;
      case PlaybackAction::Stop:
        handlePlaybackControlCommand(PlaybackControlCommand::Stop);
        break;
      case PlaybackAction::Previous:
        handlePlaybackControlCommand(PlaybackControlCommand::Previous);
        break;
      case PlaybackAction::Next:
        handlePlaybackControlCommand(PlaybackControlCommand::Next);
        break;
      case PlaybackAction::ToggleWindow:
        toggleWindowPresentation();
        break;
      case PlaybackAction::TogglePictureInPicture:
      case PlaybackAction::DismissPictureInPicture:
        togglePictureInPicture();
        break;
      case PlaybackAction::ToggleFullscreen:
        if (mediaCoordinator.toggleFullscreen()) markLayoutDirty();
        break;
      case PlaybackAction::ToggleRadio:
        toggleRadio();
        break;
      case PlaybackAction::Toggle50Hz:
        toggle50Hz();
        break;
      case PlaybackAction::ToggleOptions:
        toggleOptions();
        break;
      case PlaybackAction::TogglePitchMonitor:
        togglePitchMonitor();
        break;
      case PlaybackAction::SeekBackward:
        audioPlayback.seekBy(-1);
        markDirty();
        break;
      case PlaybackAction::SeekForward:
        audioPlayback.seekBy(1);
        markDirty();
        break;
      case PlaybackAction::VolumeUp:
        audioPlayback.adjustVolume(0.10f);
        markDirty();
        break;
      case PlaybackAction::VolumeDown:
        audioPlayback.adjustVolume(-0.10f);
        markDirty();
        break;
      case PlaybackAction::ToggleSubtitles:
      case PlaybackAction::ToggleAudioTrack:
      case PlaybackAction::PreviousFrame:
      case PlaybackAction::NextFrame:
      case PlaybackAction::CopyVideoFrame:
      case PlaybackAction::OpenVideoEditor:
      case PlaybackAction::RequestCloseVideoEditor:
      case PlaybackAction::NavigateBackInVideoEditor:
      case PlaybackAction::ConfirmVideoEditPrompt:
      case PlaybackAction::SetVideoEditIn:
      case PlaybackAction::SetVideoEditOut:
      case PlaybackAction::ClearVideoEditIn:
      case PlaybackAction::ClearVideoEditOut:
      case PlaybackAction::ClearVideoEditInAndOut:
      case PlaybackAction::RippleDeleteVideoEditSelection:
      case PlaybackAction::TrimVideoEditSelection:
      case PlaybackAction::UndoVideoEdit:
      case PlaybackAction::RedoVideoEdit:
      case PlaybackAction::ResetVideoEdits:
      case PlaybackAction::ExportVideoEdits:
      case PlaybackAction::DiscardVideoEditsAndExit:
      case PlaybackAction::CancelVideoEditPrompt:
      case PlaybackAction::ExitPlaybackSession:
      case PlaybackAction::CloseViewer:
        break;
    }
  };
  auto dispatchPlaybackCommand = [&](playback_input::Command command) {
    if (const auto* action = std::get_if<PlaybackAction>(&command)) {
      dispatchPlaybackShortcut(*action);
      return;
    }
    if (const auto* seek =
            std::get_if<playback_input::SeekToRatio>(&command)) {
      if (mediaCoordinator.seekToRatio(seek->ratio)) markDirty();
      return;
    }
    const auto* volume = std::get_if<playback_input::AdjustVolume>(&command);
    if (volume) {
      audioPlayback.adjustVolume(volume->delta);
      markDirty();
    }
  };

  const AudioPictureInPictureWindow::Styles audioPictureInPictureStyles =
      theme.audioPictureInPictureStyles();
  auto buildAudioPictureInPictureContext = [&]() {
    const PlaybackPresentationModel presentation = playbackPresenter.model();
    AudioPictureInPictureWindow::Context context;
    context.nowPlayingTarget = presentation.audioTarget;
    context.nowPlayingLabel = buildPlaybackLabel(context.nowPlayingTarget);
    context.playback = presentation.audio;
    return context;
  };
  auto renderAudioPictureInPicture = [&]() {
    if (!audioPictureInPicture.isOpen()) {
      return;
    }
    audioPictureInPicture.render(audioPictureInPictureStyles,
                                 buildAudioPictureInPictureContext());
  };
  auto handleAudioPictureInPictureEvent =
      [&](AudioPictureInPictureWindow::Event event) {
        std::visit(
            [&](auto&& value) {
              using Event = std::decay_t<decltype(value)>;
              if constexpr (std::is_same_v<
                                Event,
                                AudioPictureInPictureWindow::PlaybackCommand>) {
                dispatchPlaybackCommand(std::move(value.command));
              } else if constexpr (
                  std::is_same_v<Event,
                                 AudioPictureInPictureWindow::OpenFiles>) {
                PlaybackPresentationState videoPresentation =
                    videoConfig.enableAscii
                        ? PlaybackPresentationState::terminalAscii()
                        : PlaybackPresentationState::nativeWindowed();
                videoPresentation =
                    videoPresentation.togglePictureInPicture();
                mediaCoordinator.startDroppedFiles(
                    value.files, &value.sourcePlacement, videoPresentation);
              } else if constexpr (
                  std::is_same_v<Event,
                                 AudioPictureInPictureWindow::Closed>) {
                markDirty(UiDirtyFlags::Async);
              }
            },
            std::move(event));
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

  auto syncShellControls = [&]() {
    const PlaybackPresentationModel presentation = playbackPresenter.model();
    if (!presentation.control) {
      systemControls.clear();
      notificationAreaControls.clear();
      return;
    }
    systemControls.update(*presentation.control);
    notificationAreaControls.update(*presentation.control);
  };

  auto handleNotificationAreaCommand =
      [&](const PlaybackNotificationAreaCommand& command) {
    switch (command.kind) {
      case PlaybackNotificationAreaCommand::Kind::Activate:
        activateRadioifySurface();
        break;
      case PlaybackNotificationAreaCommand::Kind::Playback:
        handlePlaybackControlCommand(command.playbackCommand);
        break;
      case PlaybackNotificationAreaCommand::Kind::Quit:
        dispatchPlaybackShortcut(PlaybackAction::Quit);
        break;
    }
  };

  auto processShellPlaybackCommands = [&]() {
    PlaybackControlCommand command;
    while (systemControls.pollCommand(&command)) {
      handlePlaybackControlCommand(command);
    }
    PlaybackNotificationAreaCommand notificationCommand;
    while (notificationAreaControls.pollCommand(&notificationCommand)) {
      handleNotificationAreaCommand(notificationCommand);
    }
  };
  auto handleResize = [&]() {
    screenSizeDirty = true;
    markLayoutDirty();
  };
  auto handleTuiInputCommand = [&](tui_input::Command command) {
    std::visit(
        [&](auto&& value) {
          using Command = std::decay_t<decltype(value)>;
          if constexpr (
              std::is_same_v<Command, tui_input::PlaybackCommand>) {
            dispatchPlaybackCommand(std::move(value.command));
          } else if constexpr (std::is_same_v<Command, tui_input::Resize>) {
            handleResize();
          } else if constexpr (
              std::is_same_v<Command, tui_input::ActivateEntry>) {
            activateBrowserEntry(value.entry);
          } else if constexpr (
              std::is_same_v<Command, tui_input::OpenFileContextMenu>) {
            openFileContextMenu(value.entry, value.x, value.y);
          } else if constexpr (
              std::is_same_v<Command, tui_input::RenderFile>) {
            renderInputFile(value.file);
          }
        },
        std::move(command));
  };

  auto buildCommands = [&]() {
    const PlaybackPresentationModel presentation = playbackPresenter.model();
    const AudioPlaybackSnapshot& audio = presentation.audio;
    shell_command_catalog::Context context;
    context.videoActive =
        presentation.control && presentation.control->isVideo;
    context.audioAvailable = audio.source || audio.ready;
    context.pictureInPictureOpen = audioPictureInPicture.isOpen();
    context.supports50Hz = audio.supports50HzToggle;
    context.pitchMonitorActive = melodyVisualization.active();
    context.optionsAvailable =
        optionsBrowserIsActive(browser) ||
        selectedOptionsSubject().has_value();
    context.currentTargetAvailable =
        presentation.currentTarget.has_value();
    return shell_command_catalog::build(context);
  };

  auto dispatchPaletteIntent =
      [&](const shell_command_catalog::Intent& intent) {
    std::visit(
        [&](const auto& value) {
          using Intent = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Intent, PlaybackAction>) {
            dispatchPlaybackShortcut(value);
          } else if constexpr (
              std::is_same_v<Intent,
                             shell_command_catalog::SetBrowserView>) {
            browser.viewMode = value.mode;
            markLayoutDirty();
          } else if constexpr (
              std::is_same_v<Intent,
                             shell_command_catalog::RevealPlayingFile>) {
            const PlaybackPresentationModel current =
                playbackPresenter.model();
            if (current.currentTarget) {
              browserPlaybackRevealer.reveal(*current.currentTarget);
            }
          }
        },
        intent);
  };

  auto runFileContextAction = [&](tui_browser_media_menu::Command command) {
    const BrowserEntry& entry = command.entry;
    const playback_media_actions::Action action = command.action;
    dirty = true;
    const auto* track = entry.actionAs<browser_entry::PlayTrack>();
    const std::optional<int> trackIndex =
        track ? std::optional<int>(track->trackIndex) : std::nullopt;
    const media_processing::ActionRequest processingRequest =
        media_processing::captureActionRequest(
            action, entry.path, trackIndex, o.output, audioPlayback);
    const std::optional<playback_media_processing::ActionResult> processing =
        mediaActions.execute(processingRequest);
    if (processing) {
      mediaCommandError =
          processing->accepted ? std::string() : processing->feedback;
      if (processing->accepted) {
        markDirty(UiDirtyFlags::Async);
      }
      markLayoutDirty();
      return;
    }

    switch (action) {
      case playback_media_actions::Action::Play:
        if (playBrowserEntry(entry)) {
          markDirty(UiDirtyFlags::Async);
        }
        return;
      case playback_media_actions::Action::BrowseTracks:
        browserNavigator.navigate(
            browserTrackLocation(normalizeTrackBrowserPath(entry.path)));
        return;
      case playback_media_actions::Action::EditVideo: {
        playback_route::Route route =
            playback_route::resolveTarget(playbackFileTarget(entry.path));
        route.sessionIntent = PlaybackSessionIntent::EditVideo;
        const PlaybackTarget target = route.target;
        if (startPlayback(std::move(route),
                          playback_queue::singleSource(target))) {
          markDirty(UiDirtyFlags::Async);
        }
        return;
      }
      case playback_media_actions::Action::AnalyzeAudio:
      case playback_media_actions::Action::SplitLoop:
      case playback_media_actions::Action::GenerateSubtitles:
      case playback_media_actions::Action::CancelSubtitleGeneration:
      case playback_media_actions::Action::SeparateAudio:
      case playback_media_actions::Action::CancelAudioSeparation:
        return;
    }
  };

  PlaybackShellTerminalRole previousTerminalRole =
      mediaCoordinator.terminalRole();
  while (running) {
    TuiMediaCoordinator::PollResult mediaUpdate = mediaCoordinator.poll();
    for (TuiMediaCoordinator::Event& event : mediaUpdate.events) {
      handleMediaCoordinatorEvent(std::move(event));
    }
    media_processing::PollResult taskUpdate = mediaProcessing.poll();
    for (const media_processing::TaskCompletion& completion :
         taskUpdate.completions) {
      mediaCoordinator.handleMediaTaskCompletion(completion);
    }
    if (!taskUpdate.completions.empty()) {
      markLayoutDirty();
    } else if (mediaUpdate.playbackChanged || taskUpdate.changed) {
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
    while (std::optional<BrowserContentService::Completion>
               browserContentCompletion = browserContentService.poll()) {
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
    consumeBrowserNavigationEvents();
    if (browserSelectionMetadata.poll()) {
      markDirty(UiDirtyFlags::Async);
    }
    if (layoutDirty) {
      rebuildLayout();
    }
    if (windowTuiEnabled && tuiWindow.IsOpen()) {
      tuiWindow.PollEvents();
    }
    if (audioPictureInPicture.isOpen()) {
      AudioPictureInPictureWindow::PollResult update =
          audioPictureInPicture.pollEvents();
      for (AudioPictureInPictureWindow::Event& event : update.events) {
        handleAudioPictureInPictureEvent(std::move(event));
      }
      if (update.windowChanged) markDirty(UiDirtyFlags::Async);
    }
    processShellPlaybackCommands();
    if (!running) break;

    if (browserThumbnails.consumeReady()) {
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
        if (handleGlobalShellShortcut(playbackEvent)) {
          continue;
        }
        mediaCoordinator.handleVideoInputEvent(playbackEvent);
        continue;
      }
      dirty = false;
      dirtyFlags = UiDirtyFlags::None;
      forceFullRedraw = false;
      const std::vector<NativeWaitHandle> activityHandles =
          mediaWaitHandles();
      waitForBrowserWake(
          input, openFileRequests, browserThumbnails.waitHandle(),
          browserContentService.nativeWaitHandle(),
          browserSelectionMetadata.nativeWaitHandle(),
          notificationAreaControls.nativeWaitHandle(), tuiWindow,
          audioPictureInPicture, activityHandles,
          mediaCoordinator.nextWakeDeadline());
      continue;
    }

    auto processInputEvent = [&](InputEvent ev) {
      if (ev.type == InputEvent::Type::Mouse) {
        browserDoubleClickTracker.classifyUsingSystemSettings(
            ev.mouse, screen.cellPixelWidth(), screen.cellPixelHeight());
      } else {
        browserDoubleClickTracker.reset();
      }
      if (handleGlobalShellShortcut(ev)) {
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
          isCommittedFileDropEvent(ev.fileDrop) &&
          playOpenFilesRequest({ev.fileDrop.files})) {
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
      const auto browserShellAction = tui_shell_shortcuts::resolve(
          ev, tui_shell_shortcuts::context(
                  tui_shell_shortcuts::Context::Browser));
      if (browserShellAction ==
          tui_shell_shortcuts::Action::ToggleCommandPalette) {
        shellOverlays.toggleCommandPalette();
        dirty =
            setBrowserSearchFocus(browser, BrowserSearchFocus::None) || dirty;
        markDirty();
        return;
      }
      if (shellOverlays.active()) {
        const shell_command_catalog::Catalog catalog = buildCommands();
        const shell_overlay_stack::Interaction interaction =
            shellOverlays.handle(
                ev, shell_overlay_stack::Bounds{width, height, listTop},
                catalog);
        if (interaction.mediaCommand) {
          runFileContextAction(std::move(*interaction.mediaCommand));
        }
        if (interaction.paletteIntent) {
          dispatchPaletteIntent(*interaction.paletteIntent);
        }
        if (interaction.changed) {
          dirty = true;
        }
        if (interaction.consumed) {
          return;
        }
      }
      const bool browserInteractionEnabled = !melodyVisualization.active();
      const bool isLeftClick = ev.type == InputEvent::Type::Mouse &&
                               isMouseButtonDown(ev.mouse,
                                                 MouseButton::Left);
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
      if (ev.type == InputEvent::Type::Mouse && clearBtnHover && isLeftClick) {
        if (browserFilterFocused(browser)) {
          browser.filter.clear();
          dirty = setBrowserSearchFocus(browser, BrowserSearchFocus::Filter) ||
                  dirty;
        } else if (browserPathSearchFocused(browser)) {
          browser.pathSearch.clear();
          dirty =
              setBrowserSearchFocus(browser, BrowserSearchFocus::PathSearch) ||
              dirty;
        } else {
          browser.filterBackup = browser.filter;
          browser.filter.clear();
          dirty = setBrowserSearchFocus(browser, BrowserSearchFocus::Filter) ||
                  dirty;
        }
        browserNavigator.reload();
        markDirty();
        return;
      }
      if (mediaCoordinator.videoActive() && !browserSearchFocused(browser) &&
          (ev.type == InputEvent::Type::Key ||
           ev.type == InputEvent::Type::Action)) {
        const std::optional<PlaybackAction> action =
            resolveLiveBrowserVideoShortcut(ev);
        if (action && mediaCoordinator.handleVideoInputEvent(ev)) {
          markDirty(UiDirtyFlags::Async);
          return;
        }
      }
      BrowserInputResult inputResult = handleInputEvent(
          ev, browserNavigator, browserInteraction,
          BrowserInputLayout{layout, breadcrumbLine, breadcrumbY, searchBarY,
                             searchBarWidth, listTop, listHeight, progressBarX,
                             progressBarY, progressBarWidth, actionStrip},
          BrowserInputCapabilities{browserInteractionEnabled, o.play,
                                   audioPlayback.ready()});
      if (inputResult.dirty) markDirty();
      if (inputResult.quitRequested) running = false;
      for (tui_input::Command& command : inputResult.commands) {
        handleTuiInputCommand(std::move(command));
      }
    };

    auto finalizeRenderedExit = [&]() {
      audioPictureInPicture.close();
      if (windowTuiEnabled && tuiWindow.IsOpen()) {
        tuiWindow.Close();
      }
    };

    auto flushLayoutIfNeeded = [&]() {
      consumeBrowserNavigationEvents();
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
    const bool preInputMelodyVisualization = melodyVisualization.active();
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
    consumeBrowserNavigationEvents();
    browser_chrome::Model nextBrowserChrome = buildBrowserChrome();
    if (nextBrowserChrome.footer != browserChrome.footer) {
      markLayoutDirty();
    }
    browserChrome = std::move(nextBrowserChrome);

    if (browser.viewMode != preInputViewMode ||
        melodyVisualization.active() != preInputMelodyVisualization ||
        optionsBrowserIsActive(browser) != preInputOptionsMode ||
        isTrackBrowserActive(browser) != preInputTrackMode) {
      markLayoutDirty();
    }

    auto now = std::chrono::steady_clock::now();
    auto computeWakeDeadline = [&](wake_schedule::TimePoint nowTime) {
      const PlaybackPresentationModel presentation = playbackPresenter.model();
      browser_wake_schedule::Activity activity;
      activity.searchCaretVisible =
          viewport.browserInteractionEnabled &&
          browserSearchFocused(browser);
      activity.melodyMonitorVisible = melodyVisualization.active();
      activity.transportProgressVisible =
          o.play &&
          (presentation.audio.ready || presentation.currentTarget.has_value());
      activity.audioPictureInPictureVisible =
          audioPictureInPicture.isOpen();

      wake_schedule::Deadline deadline = browser_wake_schedule::nextDeadline(
          nowTime, lastDraw, activity);
      wake_schedule::include(deadline,
                             mediaCoordinator.nextWakeDeadline());
      return deadline;
    };

    if (!dirty) {
      const wake_schedule::Deadline wakeDeadline = computeWakeDeadline(now);
      const std::vector<NativeWaitHandle> activityHandles =
          mediaWaitHandles();
      DWORD waitResult = waitForBrowserWake(
          input, openFileRequests, browserThumbnails.waitHandle(),
          browserContentService.nativeWaitHandle(),
          browserSelectionMetadata.nativeWaitHandle(),
          notificationAreaControls.nativeWaitHandle(), tuiWindow,
          audioPictureInPicture, activityHandles, wakeDeadline);
      if (browserThumbnails.consumeReady()) {
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
      const PlaybackPresentationModel presentation = playbackPresenter.model();
      const AudioPlaybackSnapshot& audio = presentation.audio;
      bool optionsMode = optionsBrowserIsActive(browser);
      bool trackMode = isTrackBrowserActive(browser);
      bool browserInteractionEnabled = !melodyVisualization.active();

      screen.clear(theme.normal);
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
      Color headerBg = lerpColor(theme.header.bg, theme.headerHot.bg,
                                 std::min(0.85f, t * 0.9f));
      headerBg = lerpColor(headerBg, Color{52, 44, 26}, flash * 0.7f);
      Style headerLineStyle{theme.header.fg, headerBg};
      screen.writeRun(0, 0, width, L' ', headerLineStyle);

      Color titleFg;
      if (t < 0.35f) {
        titleFg = lerpColor(theme.header.fg, theme.headerGlow.fg, t / 0.35f);
      } else {
        float hotT = (t - 0.35f) / 0.65f;
        titleFg =
            lerpColor(theme.headerGlow.fg, theme.headerHot.fg, clamp01(hotT));
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
        const bool searchFocused = browserSearchFocused(browser);
        Style searchStyle =
            searchFocused
                ? theme.searchBarActive
                : (browserInteraction.searchBarHover ? theme.searchBarGlow
                                                     : theme.searchBar);
        screen.writeRun(0, searchBarY, width, L' ', searchStyle);
        const bool showSearchCursor =
            searchFocused && browser_wake_schedule::searchCaretOn(now);
        const bool usingPathSearch = browserPathSearchFocused(browser);
        const std::string searchText =
            usingPathSearch
                ? browser.pathSearch
                : (browser.filter.empty() ? "type to filter" : browser.filter);
        const std::string searchLine =
            std::string(usingPathSearch ? " Path: " : " Search: ") + searchText;
        int searchTextWidth = std::max(1, width - searchBarClearButtonWidth);
        std::string shownSearchLine = fitLine(searchLine, searchTextWidth);
        screen.writeText(0, searchBarY, shownSearchLine, searchStyle);
        if (showSearchCursor && searchTextWidth > 0) {
          int cursorX =
              std::min(searchTextWidth - 1, utf8DisplayWidth(shownSearchLine));
          screen.writeChar(cursorX, searchBarY, L'\u2588', searchStyle);
        }
        if (searchBarClearStart >= 0 &&
            searchBarClearEnd > searchBarClearStart) {
          Style clearStyle =
              searchBarClearHover ? theme.searchBarActive : searchStyle;
          screen.writeText(
              searchBarClearStart, searchBarY,
              fitLine(" [x] ", searchBarClearEnd - searchBarClearStart),
              clearStyle);
        }

        if (browserInteraction.breadcrumbHover >=
            static_cast<int>(breadcrumbLine.crumbs.size())) {
          browserInteraction.breadcrumbHover = -1;
        }
        screen.writeText(0, breadcrumbY, breadcrumbLine.text, theme.accent);
        if (browserInteraction.breadcrumbHover >= 0) {
          const auto& crumb = breadcrumbLine.crumbs[static_cast<size_t>(
              browserInteraction.breadcrumbHover)];
          std::string hoverText = utf8SliceDisplayWidth(
              breadcrumbLine.text, crumb.startX, crumb.endX - crumb.startX);
          screen.writeText(crumb.startX, breadcrumbY, hoverText,
                           theme.breadcrumbHover);
        }
      } else {
        browserInteraction.breadcrumbHover = -1;
      }
      const std::optional<PlaybackTarget>& nowPlayingTarget =
          presentation.currentTarget;
      const std::filesystem::path nowPlaying =
          nowPlayingTarget ? playbackTargetFile(*nowPlayingTarget)
                           : std::filesystem::path{};
      const std::optional<int> nowPlayingTrackIndex =
          nowPlayingTarget ? playbackTargetTrackIndex(*nowPlayingTarget)
                           : std::nullopt;
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
                         fitLine(showingLabel, width), theme.dim);
      }
      tui_melody_visualization::Observation melodyObservation;
      melodyObservation.source.file = nowPlaying;
      melodyObservation.source.trackIndex = nowPlayingTrackIndex;
      melodyObservation.pitch = audioPlayback.melodyInfo();
      melodyObservation.analysis = audioPlayback.melodyAnalysisState();
      melodyObservation.playbackAdvancing =
          !audio.paused && !audio.holding;
      melodyVisualization.update(std::move(melodyObservation));
      if (browserInteractionEnabled) {
        const int playingEntryIndex =
            nowPlayingTarget
                ? findBrowserPlaybackTargetEntry(browser.entries,
                                                 *nowPlayingTarget)
                : -1;
        drawBrowserEntries(screen, browserThumbnails, browser, layout, listTop,
                           listHeight,
                           theme.normal, theme.normal, theme.directory,
                           theme.highlight, theme.browserHover, theme.dim,
                           theme.accent,
                           playingEntryIndex, isSupportedImageExt, isVideoExt,
                           isSupportedAudioExt);
      } else {
        tui_melody_visualization::Bounds melodyBounds;
        melodyBounds.top = listTop;
        melodyBounds.height = listHeight;
        melodyBounds.width = width;
        tui_melody_visualization::draw(screen, melodyVisualization,
                                       melodyBounds,
                                       melodyVisualizationStyles);
      }

      int footerStart = listTop + listHeight;
      int line = footerStart;
      if (line < height && browserChrome.footer.showMeta) {
        std::string meta;
        Style metaStyle = theme.dim;
        if (browser.contentLoading) {
          meta = " Loading...";
        } else if (!browser.contentError.empty()) {
          meta = " Error: " + browser.contentError;
          metaStyle = theme.alert;
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
      if (line < height && browserChrome.footer.showWarning) {
        if (!mediaCommandError.empty()) {
          screen.writeText(
              0, line++, fitLine("  Error: " + mediaCommandError, width),
              theme.alert);
        } else {
          std::string warning = audioPlayback.warning();
          if (!warning.empty()) {
          screen.writeText(0, line++, fitLine("  Warning: " + warning, width),
                           theme.dim);
          }
        }
      }
      if (line < height && browserChrome.footer.showMediaTaskStatus) {
        const std::optional<MediaTaskStatusModel> status =
            mediaTaskPresenter.latestStatus();
        if (status) {
          if (!status->text.empty()) {
            screen.writeText(
                0, line++, fitLine(" " + status->text, width),
                status->succeeded ? theme.dim : theme.alert);
          }
        }
      }
      const std::string& nowLabel = browserChrome.nowPlayingLabel;
      if (browserChrome.footer.showNowPlaying) {
        const int nowStart = line;
        const int nowPlayingLines =
            std::max(1, browserChrome.footer.nowPlayingLines);
        std::vector<std::string> lines =
            wrapLine(std::string(" ") + nowLabel, width);
        for (int i = 0; i < nowPlayingLines &&
                        i < static_cast<int>(lines.size());
             ++i) {
          const int y = nowStart + i;
          if (y >= height) break;
          screen.writeText(0, y, lines[static_cast<size_t>(i)],
                           theme.accent);
        }
        line = nowStart + nowPlayingLines;
      }

      actionStrip.buttons.clear();
      actionStrip.y = -1;
      if (browserChrome.footer.showActionStrip && line < height) {
        actionStrip.y = line;
        const int gapWidth = 2;
        int x = 0;
        int itemLine = line;
        for (const browser_action_strip::Item& item : browserChrome.actions) {
          int widthUsed = std::min(std::max(1, item.width), width);
          const int gap = x > 0 ? gapWidth : 0;
          if (x > 0 && x + gap + widthUsed > width) {
            ++itemLine;
            x = 0;
          } else {
            x += gap;
          }
          if (itemLine >= height) break;
          bool hovered = (browserInteraction.actionHover ==
                          static_cast<int>(actionStrip.buttons.size()));
          std::string text = hovered ? item.hoverLabel : item.label;
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
          Style style = item.active ? theme.actionActive : theme.normal;
          screen.writeText(x, itemLine, text, style);
          x += widthUsed;
        }
        if (browserInteraction.actionHover >=
            static_cast<int>(actionStrip.buttons.size())) {
          browserInteraction.actionHover = -1;
        }
        line += std::max(1, browserChrome.footer.actionStripLines);
      } else {
        browserInteraction.actionHover = -1;
      }

      int peakMeterY = -1;
      if (browserChrome.footer.showPeakMeter && line < height) {
        peakMeterY = line;
        line++;
      }

      const std::optional<PlaybackControlState>& controlState =
          presentation.control;
      const bool videoActive = controlState && controlState->isVideo;
      const bool audioReady = audio.ready;
      double currentSec = controlState
                              ? controlState->positionSec
                              : audio.positionSec;
      double totalSec = controlState
                            ? controlState->durationSec.value_or(-1.0)
                            : audio.durationSec;
      double displaySec = currentSec;
      if (!videoActive && audioReady && audio.seeking) {
        const double seekSec = audio.seekTargetSec;
        if (seekSec >= 0.0 && std::isfinite(seekSec)) {
          displaySec = seekSec;
        }
      }
      const int volPct =
          static_cast<int>(std::round(audio.volume * 100.0f));
      double ratio = 0.0;
      if (totalSec > 0.0 && std::isfinite(totalSec)) {
        ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
      }
      const ProgressFooterStyles footerStyles = theme.progressFooterStyles();
      ProgressFooterInput footerInput;
      footerInput.displaySec = displaySec;
      footerInput.totalSec = totalSec;
      footerInput.ratio = ratio;
      footerInput.volPct = volPct;
      footerInput.width = width;
      footerInput.progressY = line;
      footerInput.peakY = peakMeterY;
      footerInput.unclippedOutputPeak = audio.unclippedOutputPeak;
      ProgressFooterRenderResult footerResult =
          renderProgressFooter(screen, footerInput, footerStyles);
      progressBarX = footerResult.progressBarX;
      progressBarY = footerResult.progressBarY;
      progressBarWidth = footerResult.progressBarWidth;

      if (shellOverlays.active()) {
        const shell_command_catalog::Catalog catalog = buildCommands();
        shell_overlay_stack::draw(
            screen, shellOverlays, catalog,
            shell_overlay_stack::Bounds{width, height, listTop},
            shellOverlayStyles);
      }

      if (const std::optional<MediaTaskCardModel> taskCard =
              mediaTaskPresenter.activeCard()) {
        drawMediaTaskCard(screen, width, height, listTop, *taskCard,
                          theme.mediaTaskCardStyles());
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
  screen.clear(theme.normal);
  screen.draw();
  audioPictureInPicture.close();
  if (windowTuiEnabled && tuiWindow.IsOpen()) {
    tuiWindow.Close();
  }
  input.restore();
  screen.restore();
  std::cout << "\n";
  return 0;
}
