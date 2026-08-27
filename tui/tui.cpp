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
#include <utility>
#include <variant>
#include <vector>

#include "app_common.h"
#include "app/playback_queue.h"
#include "app/playback_route.h"
#include "audio/analysis/melody_artifact_paths.h"
#include "audio/loopsplit/loopsplit.h"
#include "audio_picture_in_picture_window.h"
#include "audioplayback.h"
#include "browser_action_strip.h"
#include "browser_chrome.h"
#include "browser_media_menu.h"
#include "browser_media_menu_renderer.h"
#include "browser_playback_reveal.h"
#include "browser_playback_source.h"
#include "browser_content_preparation.h"
#include "browser_navigation.h"
#include "browser_model.h"
#include "browsermeta.h"
#include "command_palette.h"
#include "command_palette_renderer.h"
#include "consoleinput.h"
#include "consolescreen.h"
#include "core/open_file_requests.h"
#include "core/latest_request_worker.h"
#include "core/windows_app_resources.h"
#include "core/windows_message_pump.h"
#include "core/windows_console_window.h"
#include "core/windows_shell_open.h"
#include "media_coordinator.h"
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
#include "loopsplit_cli.h"
#include "audio/loopsplit/output_paths.h"
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

using BrowserContentWorker =
    LatestRequestWorker<BrowserContentRequest, BrowserPreparationResult>;

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
        browser_content_preparation::Result result =
            browser_content_preparation::prepare(
                request, [&cancellation]() {
                  return cancellation.requested();
                });
        if (std::holds_alternative<
                browser_content_preparation::Cancelled>(result)) {
          return std::optional<BrowserPreparationResult>{};
        }
        if (auto* error = std::get_if<BrowserPreparationError>(&result)) {
          return std::optional<BrowserPreparationResult>(std::move(*error));
        }
        return std::optional<BrowserPreparationResult>(
            std::move(std::get<PreparedBrowserContent>(result)));
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

  AudioPlaybackRuntime audioRuntime(audioConfig);
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

  const TuiTheme theme = radioifyTuiTheme();

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
        input, screen, theme.normal, theme.accent, theme.dim, title, message,
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

  screen.clear(theme.normal);
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
  tui_melody_visualization::Model melodyVisualization;
  const tui_melody_visualization::Styles melodyVisualizationStyles =
      theme.pitchMonitorStyles();
  std::vector<ScreenCell> windowCells;
  AudioPictureInPictureWindow audioPictureInPicture;
  ConsoleInputPump consoleInputPump;
  pointer_input::MouseDoubleClickTracker browserDoubleClickTracker;
  browser_input::EntryClickTracker browserEntryClickTracker;
  BrowserPointerState browserPointerState;
  BrowserViewport viewport;
  browser_chrome::Model browserChrome;
  auto showAudioPictureInPictureOpenError = [&]() {
    const std::string detail = audioPictureInPicture.lastError().empty()
                                   ? "The picture-in-picture window did not open."
                                   : audioPictureInPicture.lastError();
    playback_dialog::showInfoDialog(
        input, screen, theme.normal, theme.accent, theme.dim,
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

  std::string mediaCommandError;
  auto openBrowserDirectory = [&](const std::filesystem::path& dir) {
    return browserNavigator.navigate(browserDirectoryLocation(dir));
  };

  PlaybackSession::Dependencies mediaSessionDependencies{
      input, screen, theme.playbackSessionAppearance()};
  TuiMediaCoordinator::Callbacks mediaCallbacks;
  mediaCallbacks.startAudio =
      [&](const std::filesystem::path& file, int trackIndex) {
        return tryStartAudioFile(file, trackIndex);
      };
  mediaCallbacks.applyAudioPictureInPicturePlan =
      applyAudioPictureInPicturePlan;
  mediaCallbacks.openBrowserDirectory = openBrowserDirectory;
  mediaCallbacks.setCommandError = [&](std::string error) {
    mediaCommandError = std::move(error);
    markDirty(UiDirtyFlags::Async);
  };
  mediaCallbacks.requestQuit = [&]() { running = false; };
  mediaCallbacks.presentationFinished = [&]() { markDirty(); };
  mediaCallbacks.activateBrowserSurface = [&]() {
    if (windowTuiEnabled && tuiWindow.IsOpen()) {
      tuiWindow.Activate();
    } else {
      activateWindowsConsoleWindow();
    }
  };
  TuiMediaCoordinator mediaCoordinator(
      {playbackQueue, mediaSessionDependencies, videoConfig, openFileRequests,
       std::move(mediaCallbacks)});
  auto cancelActiveMediaTask = [&]() {
    const bool accepted = mediaCoordinator.cancelActiveMediaTask();
    if (accepted) {
      markLayoutDirty();
      markDirty(UiDirtyFlags::Async);
    }
    return accepted;
  };
  auto currentPlaybackFile = [&]() {
    const std::optional<PlaybackTarget> target =
        mediaCoordinator.currentPlaybackTarget();
    return target ? playbackTargetFile(*target) : std::filesystem::path{};
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

  tui_browser_media_menu::Model fileContextMenu;
  const tui_popup_menu::Styles fileContextStyles = theme.popupMenuStyles();
  auto dismissFileContextMenu = [&]() { return fileContextMenu.dismiss(); };

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
    browser_chrome::Input chromeInput;
    chromeInput.audio = audioGetPlaybackSnapshot();
    chromeInput.playback = mediaCoordinator.playbackControlState();
    chromeInput.videoPresentation =
        mediaCoordinator.videoPresentationState();
    const std::optional<PlaybackTarget> playbackTarget =
        mediaCoordinator.currentPlaybackTarget();
    chromeInput.playbackTargetAvailable = playbackTarget.has_value();
    chromeInput.audioPictureInPictureOpen =
        audioPictureInPicture.isOpen();
    chromeInput.melodyVisualizationActive = melodyVisualization.active();
    chromeInput.transportUiEnabled = o.play;
    chromeInput.optionsModeActive = optionsBrowserIsActive(browser);
    chromeInput.selectedEntryHasOptions =
        selectedOptionsSubject().has_value();
    const std::optional<MediaTaskStatusModel> mediaTaskStatus =
        mediaCoordinator.latestMediaTaskStatus();
    chromeInput.hasMediaTaskStatus =
        mediaTaskStatus && !mediaTaskStatus->text.empty();
    chromeInput.hasWarning =
        !mediaCommandError.empty() || !audioGetWarning().empty();
    chromeInput.viewMode = browser.viewMode;
    chromeInput.nowPlayingLabel = buildPlaybackLabel(playbackTarget);
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
      breadcrumbHover = -1;
    } else if (breadcrumbHover >= static_cast<int>(breadcrumbLine.crumbs.size())) {
      breadcrumbHover = -1;
    }
    layoutDirty = false;
  };

  InputCallbacks callbacks;
  auto handlePlaybackControlCommand = [&](PlaybackControlCommand command) {
    if (mediaCoordinator.handleControlCommand(command)) {
      markDirty();
    }
  };
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
    mediaCoordinator.applyMediaProcessingState(entry.path, context);
    std::vector<playback_media_actions::Item> items =
        playback_media_actions::build(context);
    if (items.empty()) return;
    tui_popup_menu::Anchor anchor;
    anchor.x = x;
    anchor.y = y;
    fileContextMenu.open(entry, std::move(items), anchor);
    markDirty();
  };
  callbacks.onRenderFile = [&](const std::filesystem::path& file) {
    renderFile(file);
    didRender = true;
  };
  callbacks.onPlay = [&]() {
    handlePlaybackControlCommand(PlaybackControlCommand::Play);
  };
  callbacks.onPause = [&]() {
    handlePlaybackControlCommand(PlaybackControlCommand::Pause);
  };
  callbacks.onTogglePause = [&]() {
    handlePlaybackControlCommand(PlaybackControlCommand::TogglePause);
  };
  callbacks.onStopPlayback = [&]() {
    handlePlaybackControlCommand(PlaybackControlCommand::Stop);
  };
  callbacks.onCurrentPlaybackFile = [&]() { return currentPlaybackFile(); };
  callbacks.onPlayPrevious = [&]() {
    handlePlaybackControlCommand(PlaybackControlCommand::Previous);
  };
  callbacks.onPlayNext = [&]() {
    handlePlaybackControlCommand(PlaybackControlCommand::Next);
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
  callbacks.onTogglePitchMonitor = [&]() {
    const AudioPlaybackSnapshot audio = audioGetPlaybackSnapshot();
    if (!melodyVisualization.active() && !audio.source && !audio.ready) {
      return;
    }
    const bool active = melodyVisualization.toggle();
    if (active) {
      setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
      breadcrumbHover = -1;
      actionHover = -1;
    }
    dismissFileContextMenu();
    markLayoutDirty();
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
    if (mediaCoordinator.seekToRatio(ratio)) markDirty();
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
    const AudioPlaybackSnapshot audio = audioGetPlaybackSnapshot();
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

  const AudioPictureInPictureWindow::Styles audioPictureInPictureStyles =
      theme.audioPictureInPictureStyles();
  auto buildAudioPictureInPictureContext = [&]() {
    AudioPictureInPictureWindow::Context context;
    context.nowPlayingTarget = mediaCoordinator.audioPlaybackTarget();
    context.nowPlayingLabel = buildPlaybackLabel(context.nowPlayingTarget);
    context.playback = audioGetPlaybackSnapshot();
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
    return mediaCoordinator.startDroppedFiles(files, &sourcePlacement,
                                              videoPresentation);
  };
  audioPictureInPictureCallbacks.onClose =
      [&]() { markDirty(UiDirtyFlags::Async); };

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
    std::optional<PlaybackControlState> state =
        mediaCoordinator.playbackControlState();
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
        handlePlaybackControlCommand(command.playbackCommand);
        break;
      case PlaybackNotificationAreaCommand::Kind::Quit:
        if (callbacks.onQuit) callbacks.onQuit();
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
  callbacks.onResize = [&]() {
    screenSizeDirty = true;
    markLayoutDirty();
  };

  tui_command_palette::Model commandPalette;
  const tui_command_palette::Styles commandPaletteStyles =
      theme.commandPaletteStyles();

  auto buildCommands = [&]() {
    const AudioPlaybackSnapshot audio = audioGetPlaybackSnapshot();
    std::vector<tui_command_palette::Command> commands;
    commands.emplace_back("Play/Pause", "Space", [&]() {
      if (callbacks.onTogglePause) {
        callbacks.onTogglePause();
      }
    });
    if (mediaCoordinator.videoActive()) {
      commands.emplace_back("Window mode (framebuffer)", "Ctrl+W", [&]() {
        if (callbacks.onToggleWindow) callbacks.onToggleWindow();
      });
      commands.emplace_back("Fullscreen", "Alt+Enter", [&]() {
        if (callbacks.onToggleFullscreen) {
          callbacks.onToggleFullscreen();
        }
      });
    }
    if (mediaCoordinator.videoActive() || audioPictureInPicture.isOpen() ||
        audio.source || audio.ready) {
      commands.emplace_back("Picture-in-Picture", "Ctrl+P", [&]() {
        if (callbacks.onTogglePictureInPicture) {
          callbacks.onTogglePictureInPicture();
        }
      });
    }
    commands.emplace_back("Cycle Radio Filter", "R", [&]() {
      audioCycleRadioFilter();
      markDirty();
    });
    const bool show50Hz = audioSupports50HzToggle();
    if (show50Hz) {
      commands.emplace_back("50Hz", "H", [&]() {
        audioToggle50Hz();
        markDirty();
      });
    }
    if (melodyVisualization.active() || audio.source || audio.ready) {
      commands.emplace_back(
          melodyVisualization.active() ? "Hide Pitch Monitor"
                                       : "Show Pitch Monitor",
          "M", [&]() {
            if (callbacks.onTogglePitchMonitor) {
              callbacks.onTogglePitchMonitor();
            }
          });
    }
    if (!melodyVisualization.active()) {
      commands.emplace_back("View: Grid", "T", [&]() {
        browser.viewMode = BrowserState::ViewMode::Thumbnails;
        markLayoutDirty();
      });
      commands.emplace_back("View: List", "T", [&]() {
        browser.viewMode = BrowserState::ViewMode::ListOnly;
        markLayoutDirty();
      });
      commands.emplace_back("View: Preview", "T", [&]() {
        browser.viewMode = BrowserState::ViewMode::ListPreview;
        markLayoutDirty();
      });
      const bool selectedEntryHasOptions =
          selectedOptionsSubject().has_value();
      if (optionsBrowserIsActive(browser) || selectedEntryHasOptions) {
        commands.emplace_back("Options", "O", [&]() {
          if (callbacks.onToggleOptions) {
            callbacks.onToggleOptions();
          }
        });
      }
      if (!currentPlaybackFile().empty()) {
        commands.emplace_back("Show Playing File", "", [&]() {
          if (const std::optional<PlaybackTarget> target =
                  mediaCoordinator.currentPlaybackTarget()) {
            browserPlaybackRevealer.reveal(*target);
          }
        });
      }
    }
    commands.emplace_back("Quit", "Q", [&]() {
      if (callbacks.onQuit) callbacks.onQuit();
    });
    return commands;
  };

  auto startMelodyExport = [&](const BrowserEntry& entry) {
    if (!entry.isMedia() || !isSupportedAudioExt(entry.path)) {
      return;
    }

    const auto* track = entry.actionAs<browser_entry::PlayTrack>();
    if (track && track->trackIndex < 0) return;
    const int trackIndex = track ? track->trackIndex : 0;
    const std::filesystem::path outputPath =
        track ? melodyArtifactPathForTrack(
                    entry.path, static_cast<std::uint32_t>(trackIndex))
              : defaultMelodyArtifactPath(entry.path);
    if (mediaCoordinator.tryStartMelodyAnalysis(
            entry.path, trackIndex, outputPath)) {
      markLayoutDirty();
      markDirty(UiDirtyFlags::Async);
    }
  };

  auto runFileContextAction = [&](tui_browser_media_menu::Command command) {
    const BrowserEntry& entry = command.entry;
    const playback_media_actions::Action action = command.action;
    dirty = true;
    const std::optional<playback_media_processing::ActionResult> processing =
        mediaCoordinator.executeMediaProcessingAction(action, entry.path);
    if (processing) {
      mediaCommandError =
          processing->accepted ? std::string() : processing->feedback;
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
        startMelodyExport(entry);
        return;
      case playback_media_actions::Action::SplitLoop: {
        if (!entry.isMedia() || !isSupportedAudioExt(entry.path)) {
          return;
        }
        LoopSplitConfig splitConfig;
        const auto* track = entry.actionAs<browser_entry::PlayTrack>();
        splitConfig.trackIndex = track ? track->trackIndex : 0;
        splitConfig.kssOptions = audioGetKssOptionState();
        splitConfig.nsfOptions = audioGetNsfOptionState();
        splitConfig.vgmOptions = audioGetVgmOptionState();
        const LoopSplitOutputPaths outputPaths =
            resolveLoopSplitOutputPaths(entry.path, o.output);
        if (mediaCoordinator.tryStartLoopSplit(
                entry.path, outputPaths.stinger, outputPaths.loop,
                splitConfig)) {
          markLayoutDirty();
          markDirty(UiDirtyFlags::Async);
        }
        return;
      }
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
    const TuiMediaCoordinator::PumpResult mediaUpdate =
        mediaCoordinator.pump();
    if (mediaUpdate.layoutChanged) {
      markLayoutDirty();
    } else if (mediaUpdate.changed) {
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
          mediaCoordinator.activityWaitHandles();
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
      const bool browserInteractionEnabled = !melodyVisualization.active();
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
          if (commandPalette.active()) {
            commandPalette.dismiss();
          } else {
            commandPalette.open();
            dismissFileContextMenu();
          }
          setBrowserSearchFocus(browser, BrowserSearchFocus::None, dirty);
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
        if (fileContextMenu.active()) {
          dismissFileContextMenu();
          dirty = true;
          return;
        }
        if (commandPalette.active()) {
          commandPalette.dismiss();
          dirty = true;
          return;
        }
      }
      if (fileContextMenu.active()) {
        tui_popup_menu::Bounds popupBounds;
        popupBounds.width = width;
        popupBounds.height = height;
        popupBounds.topInset = listTop;
        const tui_browser_media_menu::Interaction interaction =
            fileContextMenu.handle(ev, popupBounds);
        if (interaction.command) {
          runFileContextAction(std::move(*interaction.command));
        }
        if (interaction.changed) {
          dirty = true;
        }
        if (interaction.consumed) {
          return;
        }
      }
      if (commandPalette.active()) {
        const std::vector<tui_command_palette::Command> commands =
            buildCommands();
        tui_command_palette::Bounds paletteBounds;
        paletteBounds.width = width;
        paletteBounds.height = height;
        paletteBounds.topInset = listTop;
        const tui_command_palette::Interaction interaction =
            commandPalette.handle(ev, commands, paletteBounds);
        if (interaction.activatedCommand &&
            *interaction.activatedCommand < commands.size()) {
          commands[*interaction.activatedCommand].run();
        }
        if (interaction.changed) {
          dirty = true;
        }
        if (interaction.consumed) {
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
      if (melodyVisualization.active()) {
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
          mediaCoordinator.activityWaitHandles();
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
        const bool browserSearchFocused =
            browser.filterActive || browser.pathSearchActive;
        Style searchStyle =
            browserSearchFocused
                ? theme.searchBarActive
                : (searchBarHover ? theme.searchBarGlow : theme.searchBar);
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
              searchBarClearHover ? theme.searchBarActive : searchStyle;
          screen.writeText(searchBarClearStart, searchBarY,
                           fitLine(" [x] ", searchBarClearEnd - searchBarClearStart),
                           clearStyle);
        }

        if (breadcrumbHover >= static_cast<int>(breadcrumbLine.crumbs.size())) {
          breadcrumbHover = -1;
        }
        screen.writeText(0, breadcrumbY, breadcrumbLine.text, theme.accent);
        if (breadcrumbHover >= 0) {
          const auto& crumb =
              breadcrumbLine.crumbs[static_cast<size_t>(breadcrumbHover)];
          std::string hoverText = utf8SliceDisplayWidth(
              breadcrumbLine.text, crumb.startX, crumb.endX - crumb.startX);
          screen.writeText(crumb.startX, breadcrumbY, hoverText,
                           theme.breadcrumbHover);
        }
      } else {
        breadcrumbHover = -1;
      }
      const std::optional<PlaybackTarget> nowPlayingTarget =
          mediaCoordinator.currentPlaybackTarget();
      const AudioPlaybackSnapshot audio = audioGetPlaybackSnapshot();
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
      melodyObservation.pitch = audioGetMelodyInfo();
      melodyObservation.analysis = audioGetMelodyAnalysisState();
      melodyObservation.playbackAdvancing =
          !audio.paused && !audio.holding;
      melodyVisualization.update(std::move(melodyObservation));
      if (browserInteractionEnabled) {
        const int playingEntryIndex =
            nowPlayingTarget
                ? findBrowserPlaybackTargetEntry(browser.entries,
                                                 *nowPlayingTarget)
                : -1;
        drawBrowserEntries(screen, browser, layout, listTop, listHeight,
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
          std::string warning = audioGetWarning();
          if (!warning.empty()) {
          screen.writeText(0, line++, fitLine("  Warning: " + warning, width),
                           theme.dim);
          }
        }
      }
      if (line < height && browserChrome.footer.showMediaTaskStatus) {
        const std::optional<MediaTaskStatusModel> status =
            mediaCoordinator.latestMediaTaskStatus();
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
        for (const browser_action_strip::Item& item :
             browserChrome.actions) {
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
        if (actionHover >= static_cast<int>(actionStrip.buttons.size())) {
          actionHover = -1;
        }
        line += std::max(1, browserChrome.footer.actionStripLines);
      } else {
        actionHover = -1;
      }

      int peakMeterY = -1;
      if (browserChrome.footer.showPeakMeter && line < height) {
        peakMeterY = line;
        line++;
      }

      const std::optional<PlaybackControlState> controlState =
          mediaCoordinator.playbackControlState();
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

      if (commandPalette.active()) {
        const std::vector<tui_command_palette::Command> commands =
            buildCommands();
        tui_command_palette::Bounds paletteBounds;
        paletteBounds.width = width;
        paletteBounds.height = height;
        paletteBounds.topInset = listTop;
        tui_command_palette::draw(screen, commandPalette, commands,
                                  paletteBounds, commandPaletteStyles);
      }

      if (fileContextMenu.active()) {
        tui_popup_menu::Bounds popupBounds;
        popupBounds.width = width;
        popupBounds.height = height;
        popupBounds.topInset = listTop;
        tui_browser_media_menu::draw(screen, fileContextMenu, popupBounds,
                                     fileContextStyles);
      }

      if (const std::optional<MediaTaskCardModel> taskCard =
              mediaCoordinator.activeMediaTaskCard()) {
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
