#include "audio_mini_player.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <utility>

#include "audioplayback.h"
#include "input_file_drop.h"
#include "playback/framebuffer/mini_player_tui.h"
#include "playback/media/artwork_catalog.h"
#include "playback/input/shortcuts.h"
#include "playback/session/window_presentation.h"
#include "runtime_helpers.h"
#include "track_browser_state.h"
#include "tracklist.h"
#include "ui_helpers.h"
#include "ui_inputlogic.h"

namespace {

constexpr int kDefaultWindowWidth = 560;
constexpr int kDefaultWindowHeight = 260;
constexpr int kMinCols = 28;
constexpr int kMinRows = 8;

std::string trackLabelForNowPlaying(const std::filesystem::path& nowPlaying,
                                    int trackIndex) {
  if (nowPlaying.empty() || trackIndex < 0) return "";

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
  return formatTrackLabel(*track, digits);
}

std::string buildDefaultNowPlayingLabel() {
  const std::filesystem::path nowPlaying = audioGetNowPlaying();
  if (nowPlaying.empty()) return "(none)";
  std::string label = toUtf8String(nowPlaying.filename());
  std::string track = trackLabelForNowPlaying(nowPlaying, audioGetTrackIndex());
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

RECT cellRectToPixels(int x, int y, int width, int height, int cellWidth,
                      int cellHeight, int maxWidth, int maxHeight) {
  RECT rect{};
  rect.left = std::clamp(x * cellWidth, 0, maxWidth);
  rect.top = std::clamp(y * cellHeight, 0, maxHeight);
  rect.right = std::clamp((x + width) * cellWidth, 0, maxWidth);
  rect.bottom = std::clamp((y + height) * cellHeight, 0, maxHeight);
  return rect;
}

}  // namespace

bool AudioMiniPlayer::isOpen() const { return window_.IsOpen(); }

bool AudioMiniPlayer::open() { return open(nullptr); }

bool AudioMiniPlayer::openWithPlacement(
    const WindowPlacementState& placement) {
  return open(&placement);
}

bool AudioMiniPlayer::open(const WindowPlacementState* initialPlacement) {
  lastError_.clear();
  if (window_.IsOpen()) return true;
  if (!window_.Open(kDefaultWindowWidth, kDefaultWindowHeight,
                    "Radioify Mini Player", false)) {
    lastError_ = "The mini-player window could not be created.";
    return false;
  }
  if (!window_.EnableFileDrop()) {
    lastError_ =
        "Windows refused the mini-player drag/drop registration.";
    window_.Close();
    return false;
  }
  window_.SetCaptureAllMouseInput(true);
  window_.SetVsync(true);
  window_.SetTextGridMinimumSize(kMinCols, kMinRows);
  WindowPlacementState placement;
  if (initialPlacement) {
    placement = *initialPlacement;
  }
  placement.fullscreenActive = false;
  placement.pictureInPictureActive = true;
  placement.pictureInPictureRestoreFullscreen = false;
  placement.textGridPresentationEnabled = true;
  placement.pictureInPictureStartedFromTerminal = false;
  playback_session_window::applyPlacement(window_, placement);
  window_.ShowWindow(true);
  refreshGridSize();
  return true;
}

void AudioMiniPlayer::close() {
  if (window_.IsOpen()) {
    window_.Close();
  }
}

bool AudioMiniPlayer::toggle() {
  if (window_.IsOpen()) {
    close();
    return true;
  }
  return open();
}

bool AudioMiniPlayer::ensureOpen() {
  return window_.IsOpen() || open();
}

WindowPlacementState AudioMiniPlayer::capturePlacement() const {
  WindowPlacementState placement;
  playback_session_window::capturePlacement(window_, placement, false);
  return placement;
}

void AudioMiniPlayer::refreshGridSize() {
  window_.GetTextGridCellSize(cellWidth_, cellHeight_);
  cellWidth_ = std::max(1, cellWidth_);
  cellHeight_ = std::max(1, cellHeight_);
  cols_ = std::max(kMinCols, window_.GetWidth() / cellWidth_);
  rows_ = std::max(kMinRows, window_.GetHeight() / cellHeight_);
  screen_.setVirtualSize(cols_, rows_);
}

void AudioMiniPlayer::refreshArtwork(const Context& context, int width,
                                     int height) {
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

void AudioMiniPlayer::drawArtworkBackground(const Styles& styles, int width,
                                            int height) {
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

bool AudioMiniPlayer::pollEvents(const Callbacks& callbacks) {
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

bool AudioMiniPlayer::render(const Styles& styles, const Context& context) {
  if (!ensureOpen()) return false;
  refreshGridSize();
  refreshArtwork(context, cols_, rows_);
  drawArtworkBackground(styles, cols_, rows_);
  controls_.clear();
  layout_ = playback_overlay::OverlayCellLayout{};
  progressX_ = -1;
  progressY_ = -1;
  progressWidth_ = 0;

  const int width = cols_;
  const int height = rows_;
  const std::string title = context.nowPlayingLabel.empty()
                                ? buildDefaultNowPlayingLabel()
                                : context.nowPlayingLabel;

  const auto now = std::chrono::steady_clock::now();
  const bool audioReady = audioIsReady();
  const bool audioSeeking = audioIsSeeking();
  const double totalSec = audioReady ? audioGetTotalSec() : -1.0;
  const double currentSec = audioReady ? audioGetTimeSec() : 0.0;
  double displaySec = currentSec;
  if (audioReady) {
    if (audioSeeking) {
      const double seekSec = audioGetSeekTargetSec();
      if (seekSec >= 0.0 && std::isfinite(seekSec)) {
        displaySec = seekSec;
        seekDisplaySec_ = seekSec;
        seekHoldActive_ = true;
        seekHoldStart_ = now;
      }
    } else if (seekHoldActive_) {
      const double diff = std::abs(currentSec - seekDisplaySec_);
      const auto holdAge = now - seekHoldStart_;
      if ((audioIsPrimed() && diff <= 0.25) ||
          holdAge > std::chrono::seconds(2)) {
        seekHoldActive_ = false;
      } else {
        displaySec = seekDisplaySec_;
      }
    }
  } else if (seekHoldActive_ &&
             now - seekHoldStart_ > std::chrono::seconds(2)) {
    seekHoldActive_ = false;
  }
  const bool audioDisplayReady = audioReady || seekHoldActive_;
  double ratio = 0.0;
  if (std::isfinite(totalSec) && totalSec > 0.0) {
    ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
  }

  playback_overlay::PlaybackOverlayInputs overlayInputs;
  overlayInputs.windowTitle = title;
  overlayInputs.audioOk = audioDisplayReady;
  overlayInputs.playPauseAvailable = audioReady && !audioIsFinished();
  overlayInputs.audioSupports50HzToggle =
      audioReady && audioSupports50HzToggle();
  overlayInputs.canPlayPrevious = true;
  overlayInputs.canPlayNext = true;
  overlayInputs.radioEnabled = audioIsRadioEnabled();
  overlayInputs.hz50Enabled = audioIs50HzEnabled();
  overlayInputs.displaySec = displaySec;
  overlayInputs.totalSec = totalSec;
  overlayInputs.volPct =
      static_cast<int>(std::round(audioGetVolume() * 100.0f));
  overlayInputs.paused = audioIsPaused();
  overlayInputs.audioFinished = audioIsFinished();
  overlayInputs.overlayVisible = true;
  overlayInputs.pictureInPictureAvailable = true;
  overlayInputs.pictureInPictureActive = true;
  playback_overlay::PlaybackOverlayState overlayState =
      playback_overlay::buildPlaybackOverlayState(overlayInputs);

  playback_overlay::OverlayCellLayoutInput layoutInput;
  layoutInput.width = width;
  layoutInput.height = height;
  layoutInput.title = title;
  layoutInput.reservedRowsAboveProgress = audioDisplayReady ? 1 : 0;

  playback_overlay::OverlayControlSpecOptions controlOptions;
  controlOptions.includeAudioTrack = false;
  controlOptions.includeSubtitles = false;
  controls_ = playback_overlay::buildOverlayControlSpecs(
      overlayState, hoverIndex_, controlOptions);

  layoutInput.controls =
      playback_overlay::buildOverlayCellControlInputs(controls_, hoverIndex_);
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
    Style style = item.active ? styles.actionActive : styles.normal;
    if (item.hovered) {
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
      audioDisplayReady && layout_.progressBarY > 0 ? layout_.progressBarY - 1
                                                    : -1;
  footerInput.peak = audioGetPeak();
  footerInput.clipAlert = audioHasClipAlert();
  ProgressFooterRenderResult footerResult =
      renderProgressFooter(screen_, footerInput, footerStyles);
  progressX_ = footerResult.progressBarX;
  progressY_ = footerResult.progressBarY;
  progressWidth_ = footerResult.progressBarWidth;
  updateInteractiveRects();

  int outW = 0;
  int outH = 0;
  if (!screen_.snapshot(cells_, outW, outH)) return false;
  playback_framebuffer_presenter::buildGpuTextGridFrameFromScreenCells(
      cells_, outW, outH, frame_);
  window_.PresentGpuTextGrid(frame_, true);
  return true;
}

void AudioMiniPlayer::updateInteractiveRects() {
  std::vector<RECT> rects;
  rects.reserve(layout_.controls.size() + 1);

  const int maxWidth = window_.GetWidth();
  const int maxHeight = window_.GetHeight();
  auto addCellRect = [&](int x, int y, int width, int height) {
    if (width <= 0 || height <= 0 || y < 0 || y >= rows_) return;
    RECT rect = cellRectToPixels(x, y, width, height, cellWidth_, cellHeight_,
                                 maxWidth, maxHeight);
    if (rect.left < rect.right && rect.top < rect.bottom) {
      rects.push_back(rect);
    }
  };

  for (const auto& item : layout_.controls) {
    addCellRect(item.x, item.y, item.width, 1);
  }
  addCellRect(progressX_, progressY_, progressWidth_, 1);

  window_.SetPictureInPictureInteractiveRects(rects);
}

void AudioMiniPlayer::holdSeekDisplay(double targetSec) {
  if (!(targetSec >= 0.0) || !std::isfinite(targetSec)) return;
  seekDisplaySec_ = targetSec;
  seekHoldActive_ = true;
  seekHoldStart_ = std::chrono::steady_clock::now();
}

void AudioMiniPlayer::handleInput(const InputEvent& ev,
                                  const Callbacks& callbacks) {
  if (ev.type == InputEvent::Type::Resize) {
    refreshGridSize();
    return;
  }

  if (dispatchFileDrop(ev, callbacks.onPlayFiles)) {
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
    playbackCallbacks.onToggleRadio = callbacks.onToggleRadio;
    playbackCallbacks.onToggle50Hz = callbacks.onToggle50Hz;
    playbackCallbacks.onSeekBy = [&](int direction) {
      if (callbacks.onSeekBy) {
        callbacks.onSeekBy(direction);
        holdSeekDisplay(audioGetSeekTargetSec());
      }
    };
    playbackCallbacks.onAdjustVolume = callbacks.onAdjustVolume;
    playbackCallbacks.onPlaybackContextShortcut =
        [&](PlaybackShortcutAction action) {
          switch (action) {
            case PlaybackShortcutAction::DismissMiniPlayer:
              close();
              if (callbacks.onClose) callbacks.onClose();
              break;
            default:
              break;
          }
        };

    const uint32_t shortcutContexts = kPlaybackShortcutContextShared |
                                      kPlaybackShortcutContextGlobal |
                                      kPlaybackShortcutContextAudioMiniPlayer;
    handlePlaybackInput(ev, playbackCallbacks, shortcutContexts);
    return;
  }

  if (ev.type != InputEvent::Type::Mouse) return;
  const MouseEvent rawMouse = ev.mouse;
  MouseEvent mouse = rawMouse;
  const bool windowMouse = isWindowMouseEvent(mouse);
  if (windowMouse) {
    const int gx = std::clamp(rawMouse.pos.X / std::max(1, cellWidth_), 0,
                              std::max(0, cols_ - 1));
    const int gy = std::clamp(rawMouse.pos.Y / std::max(1, cellHeight_), 0,
                              std::max(0, rows_ - 1));
    mouse.pos.X = static_cast<SHORT>(gx);
    mouse.pos.Y = static_cast<SHORT>(gy);
  }

  const int hitControl = controlAt(mouse.pos.X, mouse.pos.Y);
  if (mouse.eventFlags == MOUSE_MOVED && hitControl != hoverIndex_) {
    hoverIndex_ = hitControl;
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
    if (hitControl >= 0 &&
        hitControl < static_cast<int>(controls_.size())) {
      clickControl(controls_[static_cast<size_t>(hitControl)].id, callbacks);
      return;
    }
  }

  ProgressBarHitTestInput progressHit;
  progressHit.x =
      windowMouse
          ? (rawMouse.hasPixelPosition ? rawMouse.pixelX : rawMouse.pos.X)
          : mouse.pos.X;
  progressHit.y =
      windowMouse
          ? (rawMouse.hasPixelPosition ? rawMouse.pixelY : rawMouse.pos.Y)
          : mouse.pos.Y;
  progressHit.barX = progressX_;
  progressHit.barY = progressY_;
  progressHit.barWidth = progressWidth_;
  progressHit.unitWidth = windowMouse ? cellWidth_ : 1;
  progressHit.unitHeight = windowMouse ? cellHeight_ : 1;
  double ratio = 0.0;
  if (progressBarRatioAt(progressHit, &ratio)) {
    if (callbacks.onSeekToRatio) {
      callbacks.onSeekToRatio(ratio);
      holdSeekDisplay(audioGetSeekTargetSec());
    }
  }
}

bool AudioMiniPlayer::clickControl(playback_overlay::OverlayControlId control,
                                   const Callbacks& callbacks) {
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

int AudioMiniPlayer::controlAt(int x, int y) const {
  return playback_overlay::overlayCellControlAt(layout_, x, y);
}
