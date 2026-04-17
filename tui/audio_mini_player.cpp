#include "audio_mini_player.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

#include "audioplayback.h"
#include "playback/playback_media_keys.h"
#include "playback_mini_player_tui.h"
#include "runtime_helpers.h"
#include "track_browser_state.h"
#include "tracklist.h"
#include "ui_helpers.h"

namespace {

constexpr int kDefaultWindowWidth = 560;
constexpr int kDefaultWindowHeight = 260;
constexpr int kMinCols = 28;
constexpr int kMinRows = 8;
constexpr DWORD kWindowMouseFlag = 0x80000000;

std::string statusIcon() {
  if (!audioIsReady()) return "\xE2\x97\x8B";
  if (audioIsFinished()) return "\xE2\x96\xA0";
  if (audioIsPaused()) return "\xE2\x8F\xB8";
  return "\xE2\x96\xB6";
}

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

}  // namespace

bool AudioMiniPlayer::isOpen() const { return window_.IsOpen(); }

bool AudioMiniPlayer::open() {
  if (window_.IsOpen()) return true;
  if (!window_.Open(kDefaultWindowWidth, kDefaultWindowHeight,
                    "Radioify Mini Player", false)) {
    return false;
  }
  window_.SetCaptureAllMouseInput(true);
  window_.SetVsync(true);
  window_.SetPictureInPictureTextMode(true);
  window_.SetPictureInPicture(true);
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

void AudioMiniPlayer::refreshGridSize() {
  window_.GetPictureInPictureTextCellSize(cellWidth_, cellHeight_);
  cellWidth_ = std::max(1, cellWidth_);
  cellHeight_ = std::max(1, cellHeight_);
  cols_ = std::max(kMinCols, window_.GetWidth() / cellWidth_);
  rows_ = std::max(kMinRows, window_.GetHeight() / cellHeight_);
  screen_.setVirtualSize(cols_, rows_);
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
  screen_.clear(styles.normal);
  buttons_.clear();
  progressX_ = -1;
  progressY_ = -1;
  progressWidth_ = 0;

  const int width = cols_;
  const int height = rows_;
  int row = 0;
  const std::string title = context.nowPlayingLabel.empty()
                                ? buildDefaultNowPlayingLabel()
                                : context.nowPlayingLabel;
  writeFitted(screen_, 0, row++, width, " " + title, styles.accent);

  const std::string warning = audioGetWarning();
  if (!warning.empty() && row < height) {
    writeFitted(screen_, 0, row++, width, " Warning: " + warning, styles.alert);
  }

  const bool audioReady = audioIsReady();
  const double totalSec = audioReady ? audioGetTotalSec() : -1.0;
  double displaySec = audioReady ? audioGetTimeSec() : 0.0;
  if (audioReady && audioIsSeeking() && audioGetSeekTargetSec() >= 0.0) {
    displaySec = audioGetSeekTargetSec();
  }
  double ratio = 0.0;
  if (std::isfinite(totalSec) && totalSec > 0.0) {
    ratio = std::clamp(displaySec / totalSec, 0.0, 1.0);
  }

  if (row < height) {
    const int volPct =
        static_cast<int>(std::round(audioGetVolume() * 100.0f));
    ProgressTextLayout progressText =
        buildProgressTextLayout(displaySec, totalSec, statusIcon(), volPct,
                                width);
    const int barWidth = std::max(1, std::min(progressText.barWidth,
                                              std::max(1, width - 2)));
    progressX_ = 1;
    progressY_ = row;
    progressWidth_ = barWidth;
    screen_.writeChar(0, row, L'|', styles.progressFrame);
    auto barCells = renderProgressBarCells(ratio, barWidth,
                                           styles.progressEmpty,
                                           styles.progressStart,
                                           styles.progressEnd);
    for (int i = 0; i < barWidth; ++i) {
      const auto& cell = barCells[static_cast<size_t>(i)];
      screen_.writeChar(progressX_ + i, row, cell.ch, cell.style);
    }
    screen_.writeChar(progressX_ + barWidth, row, L'|',
                      styles.progressFrame);
    const int suffixX = progressX_ + barWidth + 2;
    if (!progressText.suffix.empty() && suffixX < width) {
      writeFitted(screen_, suffixX, row, width - suffixX,
                  progressText.suffix, styles.normal);
    }
    row++;
  }

  if (row < height) {
    const float peak = std::clamp(audioGetPeak(), 0.0f, 1.2f);
    const int meterWidth = std::max(6, std::min(width - 12, 24));
    screen_.writeText(0, row, " Peak ", styles.dim);
    Color meterStart = styles.progressFrame.fg;
    Color meterEnd = styles.progressFrame.fg;
    if (audioHasClipAlert() && peak >= 0.80f) {
      meterStart = styles.alert.fg;
      meterEnd = styles.alert.fg;
    } else if (peak >= 0.80f) {
      meterStart = styles.accent.fg;
      meterEnd = styles.progressEnd;
    }
    auto meterCells = renderProgressBarCells(
        std::clamp(static_cast<double>(peak), 0.0, 1.0), meterWidth,
        styles.progressEmpty, meterStart, meterEnd);
    for (int i = 0; i < meterWidth; ++i) {
      const auto& cell = meterCells[static_cast<size_t>(i)];
      screen_.writeChar(6 + i, row, cell.ch, cell.style);
    }
    row++;
  }

  if (context.melodyVisualizationEnabled && row < height) {
    const AudioMelodyInfo melody = audioGetMelodyInfo();
    const AudioMelodyAnalysisState analysis = audioGetMelodyAnalysisState();
    std::string melodyLine = " Melody ";
    if (melody.midiNote >= 0) {
      const int hz = static_cast<int>(std::round(melody.frequencyHz));
      const int pct = static_cast<int>(
          std::round(std::clamp(melody.confidence, 0.0f, 1.0f) * 100.0f));
      melodyLine += std::to_string(hz) + "Hz " + std::to_string(pct) + "%";
    } else {
      melodyLine += "--";
    }
    if (analysis.running) {
      const int pct = static_cast<int>(
          std::round(std::clamp(analysis.progress, 0.0f, 1.0f) * 100.0f));
      melodyLine += "  analysis " + std::to_string(pct) + "%";
    } else if (analysis.ready) {
      melodyLine += "  analysis ready";
    }
    writeFitted(screen_, 0, row++, width, melodyLine, styles.dim);
  }

  if (row < height) row++;

  const int gap = 1;
  int x = 0;
  auto addButton = [&](Button::Action action, const std::string& label,
                       bool active) {
    if (row >= height) return;
    std::string text = " [" + label + "] ";
    int itemWidth = utf8DisplayWidth(text);
    if (itemWidth > width && width > 0) {
      text = utf8TakeDisplayWidth(text, width);
      itemWidth = utf8DisplayWidth(text);
    }
    if (x > 0 && x + itemWidth > width) {
      row++;
      x = 0;
      if (row >= height) return;
    }
    if (itemWidth <= 0 || x >= width) return;
    const int drawWidth = std::min(itemWidth, width - x);
    std::string fitted = text;
    if (utf8DisplayWidth(fitted) > drawWidth) {
      fitted = utf8TakeDisplayWidth(fitted, drawWidth);
    }
    Button button{action, x, row, x + drawWidth};
    buttons_.push_back(button);
    screen_.writeText(x, row, fitted,
                      active ? styles.actionActive : styles.normal);
    x += drawWidth + gap;
  };

  addButton(Button::Action::Previous, "<<", false);
  addButton(Button::Action::PlayPause,
            audioIsPaused() || audioIsFinished() ? "Play" : "Pause", false);
  addButton(Button::Action::Next, ">>", false);
  addButton(Button::Action::Stop, "Stop", false);
  addButton(Button::Action::Radio,
            audioIsRadioEnabled() ? "Radio: AM" : "Radio: Dry",
            audioIsRadioEnabled());
  addButton(Button::Action::Melody,
            context.melodyVisualizationEnabled ? "Melody: On" : "Melody: Off",
            context.melodyVisualizationEnabled);
  if (audioSupports50HzToggle()) {
    addButton(Button::Action::Hz50,
              audioIs50HzEnabled() ? "50Hz: 50" : "50Hz: Auto",
              audioIs50HzEnabled());
  }
  addButton(Button::Action::Close, "PiP: On", true);

  int outW = 0;
  int outH = 0;
  if (!screen_.snapshot(cells_, outW, outH)) return false;
  playback_framebuffer_presenter::buildGpuTextGridFrameFromScreenCells(
      cells_, outW, outH, frame_);
  window_.PresentGpuTextGrid(frame_, true);
  return true;
}

void AudioMiniPlayer::handleInput(const InputEvent& ev,
                                  const Callbacks& callbacks) {
  if (ev.type == InputEvent::Type::Resize) {
    refreshGridSize();
    return;
  }

  if (ev.type == InputEvent::Type::Key) {
    const KeyEvent& key = ev.key;
    const DWORD shiftMask = SHIFT_PRESSED;
    const bool shift = (key.control & shiftMask) != 0;
    if (key.vk == VK_ESCAPE || key.vk == 'W' || key.ch == 'w' ||
        key.ch == 'W') {
      close();
      if (callbacks.onClose) callbacks.onClose();
      return;
    }
    if (key.vk == kPlaybackVkMediaPlay) {
      if (callbacks.onTogglePause) callbacks.onTogglePause();
      return;
    }
    if (key.vk == kPlaybackVkMediaPause) {
      if (callbacks.onTogglePause) callbacks.onTogglePause();
      return;
    }
    if (key.vk == VK_SPACE || key.ch == ' ' ||
        key.vk == VK_MEDIA_PLAY_PAUSE) {
      if (callbacks.onTogglePause) callbacks.onTogglePause();
      return;
    }
    if (key.vk == VK_MEDIA_STOP) {
      if (callbacks.onStopPlayback) callbacks.onStopPlayback();
      return;
    }
    if (key.vk == VK_MEDIA_PREV_TRACK) {
      if (callbacks.onPlayPrevious) callbacks.onPlayPrevious();
      return;
    }
    if (key.vk == VK_MEDIA_NEXT_TRACK) {
      if (callbacks.onPlayNext) callbacks.onPlayNext();
      return;
    }
    if (key.vk == 'R' || key.ch == 'r' || key.ch == 'R') {
      if (callbacks.onToggleRadio) callbacks.onToggleRadio();
      return;
    }
    if (key.vk == 'H' || key.ch == 'h' || key.ch == 'H') {
      if (callbacks.onToggle50Hz) callbacks.onToggle50Hz();
      return;
    }
    if (key.vk == 'M' || key.ch == 'm' || key.ch == 'M') {
      if (callbacks.onToggleMelodyVisualization) {
        callbacks.onToggleMelodyVisualization();
      }
      return;
    }
    if (key.vk == VK_OEM_4 || key.ch == '[') {
      if (callbacks.onSeekBy) callbacks.onSeekBy(-1);
      return;
    }
    if (key.vk == VK_OEM_6 || key.ch == ']') {
      if (callbacks.onSeekBy) callbacks.onSeekBy(1);
      return;
    }
    if (key.vk == VK_UP && shift) {
      if (callbacks.onAdjustVolume) callbacks.onAdjustVolume(0.10f);
      return;
    }
    if (key.vk == VK_DOWN && shift) {
      if (callbacks.onAdjustVolume) callbacks.onAdjustVolume(-0.10f);
      return;
    }
    return;
  }

  if (ev.type != InputEvent::Type::Mouse) return;
  MouseEvent mouse = ev.mouse;
  if ((mouse.control & kWindowMouseFlag) != 0) {
    const int gx = std::clamp(mouse.pos.X / std::max(1, cellWidth_), 0,
                              std::max(0, cols_ - 1));
    const int gy = std::clamp(mouse.pos.Y / std::max(1, cellHeight_), 0,
                              std::max(0, rows_ - 1));
    mouse.pos.X = static_cast<SHORT>(gx);
    mouse.pos.Y = static_cast<SHORT>(gy);
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
    for (const Button& button : buttons_) {
      if (mouse.pos.Y == button.y && mouse.pos.X >= button.x0 &&
          mouse.pos.X < button.x1) {
        clickButton(button, callbacks);
        return;
      }
    }
  }

  if (progressWidth_ > 0 && mouse.pos.Y == progressY_ &&
      mouse.pos.X >= progressX_ &&
      mouse.pos.X < progressX_ + progressWidth_) {
    if (callbacks.onSeekToRatio) {
      callbacks.onSeekToRatio(progressRatioAt(mouse.pos.X));
    }
  }
}

bool AudioMiniPlayer::clickButton(const Button& button,
                                  const Callbacks& callbacks) {
  switch (button.action) {
    case Button::Action::Previous:
      if (callbacks.onPlayPrevious) callbacks.onPlayPrevious();
      return true;
    case Button::Action::PlayPause:
      if (callbacks.onTogglePause) callbacks.onTogglePause();
      return true;
    case Button::Action::Next:
      if (callbacks.onPlayNext) callbacks.onPlayNext();
      return true;
    case Button::Action::Stop:
      if (callbacks.onStopPlayback) callbacks.onStopPlayback();
      return true;
    case Button::Action::Radio:
      if (callbacks.onToggleRadio) callbacks.onToggleRadio();
      return true;
    case Button::Action::Melody:
      if (callbacks.onToggleMelodyVisualization) {
        callbacks.onToggleMelodyVisualization();
      }
      return true;
    case Button::Action::Hz50:
      if (callbacks.onToggle50Hz) callbacks.onToggle50Hz();
      return true;
    case Button::Action::Close:
      close();
      if (callbacks.onClose) callbacks.onClose();
      return true;
  }
  return false;
}

double AudioMiniPlayer::progressRatioAt(int x) const {
  const int rel = std::clamp(x - progressX_, 0, std::max(0, progressWidth_ - 1));
  const double denom = static_cast<double>(std::max(1, progressWidth_ - 1));
  return std::clamp(static_cast<double>(rel) / denom, 0.0, 1.0);
}
