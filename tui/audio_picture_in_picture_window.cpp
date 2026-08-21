#include "audio_picture_in_picture_window.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <utility>

#include "audioplayback.h"
#include "core/windows_app_resources.h"
#include "playback/framebuffer/mini_player_tui.h"
#include "playback/media/artwork_catalog.h"
#include "playback/input/shortcuts.h"
#include "playback/session/window_presentation.h"
#include "runtime_helpers.h"
#include "tracklist.h"
#include "ui_helpers.h"
#include "ui_inputlogic.h"

namespace {

constexpr int kDefaultWindowWidth = 560;
constexpr int kDefaultWindowHeight = 260;
constexpr int kMinCols = 28;
constexpr int kMinRows = 8;

std::string trackLabelForNowPlaying(int trackIndex) {
  if (trackIndex < 0) return "";

  TrackEntry fallback{};
  fallback.index = trackIndex;
  return formatTrackLabel(fallback, 3);
}

std::string buildDefaultNowPlayingLabel() {
  const std::filesystem::path nowPlaying = audioGetNowPlaying();
  if (nowPlaying.empty()) return "(none)";
  std::string label = toUtf8String(nowPlaying.filename());
  std::string track = trackLabelForNowPlaying(audioGetTrackIndex());
  if (!track.empty()) {
    label += "  |  " + track;
  }
  return label;
}

void writeFitted(ConsoleScreen& screen, int x, int y, int width,
                 const std::string& text, const Style& style) {
  if (width <= 0) return;
  screen.writeText(x, y, fitLine(text, width), style);
}

bool leftPressed(const MouseEvent& mouse) {
  return (mouse.buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
}

int wheelDelta(const MouseEvent& mouse) {
  return static_cast<SHORT>(HIWORD(mouse.buttonState));
}

}  // namespace

bool AudioPictureInPictureWindow::isOpen() const { return window_.IsOpen(); }

NativeWaitHandle AudioPictureInPictureWindow::inputWaitHandle() const {
  return window_.InputWaitHandle();
}

NativeWaitHandle AudioPictureInPictureWindow::closeRequestedWaitHandle() const {
  return window_.CloseRequestedWaitHandle();
}

bool AudioPictureInPictureWindow::open() {
  lastError_.clear();
  if (window_.IsOpen()) return true;
  if (!window_.Open(kDefaultWindowWidth, kDefaultWindowHeight,
                    RADIOIFY_APP_NAME " Picture-in-Picture")) {
    lastError_ = "The picture-in-picture window could not be created.";
    return false;
  }
  if (!window_.EnableFileDrop()) {
    lastError_ =
        "Windows refused the picture-in-picture drag/drop registration.";
    close();
    return false;
  }
  window_.SetVsync(true);
  window_.SetTextGridMinimumSize(kMinCols, kMinRows);
  const PlaybackPresentationState presentation =
      PlaybackPresentationState::nativeWindowed(
          PlaybackVisualMode::AsciiGrid)
          .togglePictureInPicture();
  const auto request = windowPresentationRequest(
      presentation, PlaybackPresentationFocus::KeepCurrentSurface);
  if (!request || !playback_session_window::apply(window_, *request)) {
    lastError_ = "The picture-in-picture presentation could not be applied.";
    close();
    return false;
  }
  refreshGridSize();
  return true;
}

void AudioPictureInPictureWindow::close() {
  window_.Close();
  hoverControlToken_ = -1;
  controls_.clear();
  layout_ = playback_overlay::OverlayCellLayout{};
  interactions_ = {};
}

void AudioPictureInPictureWindow::activate() {
  if (window_.IsOpen()) {
    window_.Activate();
  }
}

bool AudioPictureInPictureWindow::toggle() {
  if (window_.IsOpen()) {
    close();
    return true;
  }
  return open();
}

bool AudioPictureInPictureWindow::ensureOpen() {
  return window_.IsOpen() || open();
}

WindowPlacementState AudioPictureInPictureWindow::capturePlacement() const {
  WindowPlacementState placement;
  const PlaybackPresentationState presentation =
      PlaybackPresentationState::nativeWindowed(
          PlaybackVisualMode::AsciiGrid)
          .togglePictureInPicture();
  playback_session_window::capturePlacement(window_, placement, presentation);
  return placement;
}

void AudioPictureInPictureWindow::refreshGridSize() {
  window_.GetTextGridCellSize(cellWidth_, cellHeight_);
  cellWidth_ = std::max(1, cellWidth_);
  cellHeight_ = std::max(1, cellHeight_);
  cols_ = std::max(kMinCols, window_.GetWidth() / cellWidth_);
  rows_ = std::max(kMinRows, window_.GetHeight() / cellHeight_);
  screen_.setVirtualSize(cols_, rows_);
}

void AudioPictureInPictureWindow::refreshArtwork(const Context& context,
                                                 int width, int height) {
  const bool cacheHit =
      artworkValid_ && context.nowPlayingPath == artworkPath_ &&
      context.trackIndex == artworkTrackIndex_ && width == artworkWidth_ &&
      height == artworkHeight_;
  if (cacheHit) return;

  artwork_ = AsciiArt{};
  artworkPath_ = context.nowPlayingPath;
  artworkTrackIndex_ = context.trackIndex;
  artworkWidth_ = width;
  artworkHeight_ = height;
  artworkValid_ = false;
  if (context.nowPlayingPath.empty() || width <= 0 || height <= 0) {
    return;
  }

  PlaybackMediaDisplayRequest request;
  request.file = context.nowPlayingPath;
  request.trackIndex = context.trackIndex;
  request.isVideo = false;

  std::string ignoredError;
  if (resolvePlaybackMediaArtworkAscii(
          request, MediaArtworkSidecarPolicy::IncludeGenericDirectoryFallback,
          width, height, &artwork_, &ignoredError)) {
    artworkValid_ = artwork_.width > 0 && artwork_.height > 0 &&
                    artwork_.cells.size() >=
                        static_cast<size_t>(artwork_.width) *
                            static_cast<size_t>(artwork_.height);
  }
}

void AudioPictureInPictureWindow::drawArtworkBackground(const Styles& styles,
                                                        int width, int height) {
  if (!artworkValid_ || artwork_.width <= 0 || artwork_.height <= 0) {
    screen_.clear(styles.normal);
    return;
  }

  const size_t expected =
      static_cast<size_t>(artwork_.width) * static_cast<size_t>(artwork_.height);
  if (artwork_.cells.size() < expected) {
    screen_.clear(styles.normal);
    return;
  }

  for (int y = 0; y < height; ++y) {
    const int sy =
        std::clamp((y * artwork_.height) / std::max(1, height), 0,
                   artwork_.height - 1);
    for (int x = 0; x < width; ++x) {
      const int sx =
          std::clamp((x * artwork_.width) / std::max(1, width), 0,
                     artwork_.width - 1);
      const auto& src =
          artwork_.cells[static_cast<size_t>(sy * artwork_.width + sx)];
      const Color bg = src.hasBg ? src.bg : styles.normal.bg;
      Style style{scaleColor(src.fg, 0.52f), scaleColor(bg, 0.38f)};
      screen_.writeChar(x, y, src.ch, style);
    }
  }
}

bool AudioPictureInPictureWindow::pollEvents(const Callbacks& callbacks) {
  if (!window_.IsOpen()) return false;
  bool handled = window_.PollEvents();
  if (window_.ConsumeCloseRequested()) {
    close();
    if (callbacks.onClose) callbacks.onClose();
    return true;
  }

  InputEvent ev{};
  while (window_.PollInput(ev)) {
    handled = true;
    handleInput(ev, callbacks);
  }
  return handled;
}

bool AudioPictureInPictureWindow::render(const Styles& styles,
                                         const Context& context) {
  if (!ensureOpen()) return false;
  refreshGridSize();
  refreshArtwork(context, cols_, rows_);
  drawArtworkBackground(styles, cols_, rows_);
  controls_.clear();
  layout_ = playback_overlay::OverlayCellLayout{};
  interactions_ = {};

  const int width = cols_;
  const int height = rows_;
  const std::string title = context.nowPlayingLabel.empty()
                                ? buildDefaultNowPlayingLabel()
                                : context.nowPlayingLabel;

  const bool audioReady = audioIsReady();
  const bool audioSeeking = audioIsSeeking();
  const bool audioFinished = audioIsFinished();
  const double totalSec = audioReady ? audioGetTotalSec() : -1.0;
  const double currentSec = audioReady ? audioGetTimeSec() : 0.0;
  double displaySec = currentSec;
  if (audioReady && audioSeeking) {
    const double seekSec = audioGetSeekTargetSec();
    if (seekSec >= 0.0 && std::isfinite(seekSec)) {
      displaySec = seekSec;
    }
  }
  double ratio = 0.0;
  if (std::isfinite(totalSec) && totalSec > 0.0) {
    ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
  }

  playback_overlay::PlaybackOverlayInputs overlayInputs;
  overlayInputs.windowTitle = title;
  overlayInputs.audioOk = audioReady;
  overlayInputs.playPauseAvailable = audioReady;
  overlayInputs.audioSupports50HzToggle =
      audioReady && audioSupports50HzToggle();
  overlayInputs.canPlayPrevious = true;
  overlayInputs.canPlayNext = true;
  overlayInputs.radioEnabled = audioIsRadioEnabled();
  overlayInputs.radioLabel = std::string(audioGetRadioFilterLabel());
  overlayInputs.hz50Enabled = audioIs50HzEnabled();
  overlayInputs.displaySec = displaySec;
  overlayInputs.totalSec = totalSec;
  overlayInputs.volPct =
      static_cast<int>(std::round(audioGetVolume() * 100.0f));
  overlayInputs.paused = audioIsPaused() || audioFinished;
  overlayInputs.osd.controlsVisible = true;
  overlayInputs.pictureInPictureAvailable = true;
  overlayInputs.pictureInPictureActive = true;
  playback_overlay::PlaybackOverlayState overlayState =
      playback_overlay::buildPlaybackOverlayState(overlayInputs);

  playback_overlay::OverlayCellLayoutInput layoutInput;
  layoutInput.width = width;
  layoutInput.height = height;
  layoutInput.title = title;
  layoutInput.reservedRowsAboveProgress = audioReady ? 1 : 0;

  playback_overlay::OverlayControlSpecOptions controlOptions;
  controlOptions.includeAudioTrack = false;
  controlOptions.includeSubtitles = false;
  controls_ = playback_overlay::buildOverlayControlSpecs(
      overlayState, hoverControlToken_, controlOptions);

  layoutInput.controls =
      playback_overlay::buildOverlayCellControlInputs(controls_,
                                                      hoverControlToken_);
  layout_ = playback_overlay::layoutOverlayCells(layoutInput);
  for (const auto& titleLine : layout_.titleLines) {
    if (titleLine.y < 0 || titleLine.y >= height || titleLine.x >= width) {
      continue;
    }
    writeFitted(screen_, titleLine.x, titleLine.y,
                width - titleLine.x, titleLine.text, styles.accent);
  }
  for (const auto& item : layout_.controls) {
    if (item.y < 0 || item.y >= height || item.x >= width) continue;
    Style style = item.enabled
                      ? (item.active ? styles.actionActive : styles.normal)
                      : styles.dim;
    if (item.enabled && item.hovered) {
      style = {style.bg, style.fg};
    }
    writeFitted(screen_, item.x, item.y, width - item.x, item.text, style);
  }
  ProgressFooterStyles footerStyles{styles.normal,
                                    styles.progressEmpty,
                                    styles.progressFrame,
                                    styles.alert,
                                    styles.accent,
                                    styles.progressStart,
                                    styles.progressEnd};
  ProgressFooterInput footerInput;
  footerInput.displaySec = displaySec;
  footerInput.totalSec = totalSec;
  footerInput.ratio = ratio;
  footerInput.volPct = overlayState.volPct;
  footerInput.width = width;
  footerInput.progressY = layout_.progressBarY;
  footerInput.peakY =
      audioReady && layout_.progressBarY > 0 ? layout_.progressBarY - 1 : -1;
  footerInput.unclippedOutputPeak = audioGetUnclippedOutputPeak();
  ProgressFooterRenderResult footerResult =
      renderProgressFooter(screen_, footerInput, footerStyles);
  interactions_ = playback_overlay::buildOverlayInteractionMap(layout_);
  if (footerResult.progressBarX >= 0 && footerResult.progressBarY >= 0 &&
      footerResult.progressBarWidth > 0) {
    interactions_.progressBar = playback_overlay::ProgressBarRegion{
        {static_cast<double>(footerResult.progressBarX),
         static_cast<double>(footerResult.progressBarY),
         static_cast<double>(footerResult.progressBarX +
                             footerResult.progressBarWidth),
         static_cast<double>(footerResult.progressBarY + 1)},
        footerResult.progressBarWidth};
  } else {
    interactions_.progressBar.reset();
  }

  int outW = 0;
  int outH = 0;
  if (!screen_.snapshot(cells_, outW, outH)) return false;
  playback_framebuffer_presenter::buildGpuTextGridFrameFromScreenCells(
      cells_, outW, outH, frame_);
  window_.PresentGpuTextGrid(frame_, interactions_);
  return true;
}

void AudioPictureInPictureWindow::handleInput(const InputEvent& ev,
                                  const Callbacks& callbacks) {
  if (ev.type == InputEvent::Type::Resize) {
    refreshGridSize();
    return;
  }

  if (ev.type == InputEvent::Type::FileDrop &&
      isCommittedFileDropEvent(ev.fileDrop) && callbacks.onPlayFiles &&
      callbacks.onPlayFiles(ev.fileDrop.files)) {
    return;
  }

  if (ev.type == InputEvent::Type::Key) {
    InputCallbacks playbackCallbacks;
    playbackCallbacks.onQuit = callbacks.onQuit;
    playbackCallbacks.onTogglePause = callbacks.onTogglePause;
    playbackCallbacks.onStopPlayback = callbacks.onStopPlayback;
    playbackCallbacks.onPlayPrevious = callbacks.onPlayPrevious;
    playbackCallbacks.onPlayNext = callbacks.onPlayNext;
    playbackCallbacks.onToggleWindow = [&]() {
      close();
      if (callbacks.onClose) callbacks.onClose();
    };
    playbackCallbacks.onTogglePictureInPicture =
        playbackCallbacks.onToggleWindow;
    playbackCallbacks.onToggleRadio = callbacks.onToggleRadio;
    playbackCallbacks.onToggle50Hz = callbacks.onToggle50Hz;
    playbackCallbacks.onSeekBy = [&](int direction) {
      if (callbacks.onSeekBy) {
        callbacks.onSeekBy(direction);
      }
    };
    playbackCallbacks.onAdjustVolume = callbacks.onAdjustVolume;
    playbackCallbacks.onPlaybackContextShortcut =
        [&](PlaybackShortcutAction action) {
          switch (action) {
            case PlaybackShortcutAction::DismissPictureInPicture:
              close();
              if (callbacks.onClose) callbacks.onClose();
              break;
            default:
              break;
          }
        };

    const uint32_t shortcutContexts = kPlaybackShortcutContextShared |
                      kPlaybackShortcutContextGlobal |
                      kPlaybackShortcutContextPictureInPicture;
    handlePlaybackInput(ev, playbackCallbacks, shortcutContexts);
    return;
  }

  if (ev.type != InputEvent::Type::Mouse) return;
  const MouseEvent rawMouse = ev.mouse;
  const MouseEvent& mouse = rawMouse;
  const bool windowMouse = isWindowMouseEvent(mouse);
  const double pointerX =
      windowMouse && rawMouse.hasPixelPosition ? rawMouse.pixelX
                                               : rawMouse.pos.X;
  const double pointerY =
      windowMouse && rawMouse.hasPixelPosition ? rawMouse.pixelY
                                               : rawMouse.pos.Y;
  const playback_overlay::InteractionHit interactionHit =
      windowMouse ? window_.OverlayHitAt(pointerX, pointerY)
                  : playback_overlay::interactionHitAt(
                        interactions_, pointerX, pointerY);
  const auto& hitControl = interactionHit.control;
  const int hitControlToken =
      hitControl ? playback_overlay::overlayControlToken(*hitControl) : -1;
  if (mouse.eventFlags == MOUSE_MOVED &&
      hitControlToken != hoverControlToken_) {
    hoverControlToken_ = hitControlToken;
  }

  if (mouse.eventFlags == MOUSE_WHEELED) {
    const int delta = wheelDelta(mouse);
    if (delta != 0 && callbacks.onAdjustVolume) {
      callbacks.onAdjustVolume(delta > 0 ? 0.05f : -0.05f);
    }
    return;
  }

  if (!leftPressed(mouse) ||
      (mouse.eventFlags != 0 && mouse.eventFlags != MOUSE_MOVED)) {
    return;
  }

  if (mouse.eventFlags == 0) {
    if (hitControl) {
      clickControl(*hitControl, callbacks);
      return;
    }
  }

  const auto& progressHit = interactionHit.progressBar;
  if (progressHit) {
    if (callbacks.onSeekToRatio) {
      callbacks.onSeekToRatio(progressHit->ratio);
    }
  }
}

bool AudioPictureInPictureWindow::clickControl(
    playback_overlay::OverlayControlId control, const Callbacks& callbacks) {
  auto invoke = [](const std::function<void()>& action) {
    if (!action) return false;
    action();
    return true;
  };

  playback_overlay::OverlayControlActions actions;
  actions.previous = [&]() { return invoke(callbacks.onPlayPrevious); };
  actions.playPause = [&]() { return invoke(callbacks.onTogglePause); };
  actions.next = [&]() { return invoke(callbacks.onPlayNext); };
  actions.radio = [&]() { return invoke(callbacks.onToggleRadio); };
  actions.hz50 = [&]() { return invoke(callbacks.onToggle50Hz); };
  actions.pictureInPicture = [&]() {
    close();
    if (callbacks.onClose) callbacks.onClose();
    return true;
  };
  return playback_overlay::dispatchOverlayControl(control, actions);
}
