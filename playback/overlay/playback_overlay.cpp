#include "playback_overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>

#include "ui_helpers.h"

namespace playback_overlay {
namespace {

int countVisibleChars(const std::string& text) {
  std::string filtered;
  filtered.reserve(text.size());
  for (char c : text) {
    if (c == '\r' || c == '\n') continue;
    filtered.push_back(c);
  }
  return utf8DisplayWidth(filtered);
}

std::string fitCellText(const std::string& text, int width) {
  if (width <= 0) return {};
  std::string filtered;
  filtered.reserve(text.size());
  for (char c : text) {
    if (c == '\r' || c == '\n') continue;
    filtered.push_back(c);
  }

  const int displayWidth = utf8DisplayWidth(filtered);
  if (displayWidth > width) {
    return utf8TakeDisplayWidth(filtered, width);
  }
  if (displayWidth < width) {
    filtered.append(static_cast<size_t>(width - displayWidth), ' ');
  }
  return filtered;
}

std::pair<std::string, std::string> makeLabels(const std::string& text) {
  return std::pair<std::string, std::string>(" [" + text + "] ",
                                             "[ " + text + " ]");
}

struct PendingOverlayCellControl {
  std::string text;
  int controlIndex = -1;
  int x = 0;
  int line = 0;
  int width = 0;
  bool active = false;
  bool hovered = false;
};

std::vector<PendingOverlayCellControl> wrapOverlayControls(
    const std::vector<OverlayCellControlInput>& controls, int width,
    int* outLineCount) {
  const int contentInset = width > 2 ? 1 : 0;
  const int maxLineWidth = std::max(1, width - contentInset * 2);
  std::vector<PendingOverlayCellControl> out;
  out.reserve(controls.size());

  int cursor = 0;
  int line = 0;
  for (const OverlayCellControlInput& control : controls) {
    const int controlWidth =
        std::min(maxLineWidth,
                 std::max(1, control.width > 0
                                  ? control.width
                                  : utf8DisplayWidth(control.text)));
    const int gap = cursor > 0 ? 2 : 0;
    if (cursor > 0 && cursor + gap + controlWidth > maxLineWidth) {
      ++line;
      cursor = 0;
    } else {
      cursor += gap;
    }

    PendingOverlayCellControl item;
    item.text = fitCellText(control.text, controlWidth);
    item.controlIndex = control.controlIndex;
    item.x = contentInset + cursor;
    item.line = line;
    item.width = controlWidth;
    item.active = control.active;
    item.hovered = control.hovered;
    out.push_back(std::move(item));
    cursor += controlWidth;
  }

  if (outLineCount) {
    *outLineCount = out.empty() ? 0 : line + 1;
  }
  return out;
}

struct WindowOverlayCellGeometry {
  OverlayCellLayout layout;
  int leftPx = 0;
  int topPx = 0;
  int cellWidth = 1;
  int cellHeight = 1;
};

WindowOverlayCellGeometry buildWindowOverlayCellGeometry(
    const PlaybackOverlayState& state, int cellPixelWidth,
    int cellPixelHeight) {
  WindowOverlayCellGeometry geometry;
  geometry.cellWidth = std::max(1, cellPixelWidth);
  geometry.cellHeight = std::max(1, cellPixelHeight);
  if (state.windowWidth <= 0 || state.windowHeight <= 0) {
    return geometry;
  }

  const int maxTextWidth =
      std::clamp(static_cast<int>(std::lround(state.windowWidth * 0.96)), 1,
                 state.windowWidth);
  const int cols = std::max(1, maxTextWidth / geometry.cellWidth);
  geometry.layout = layoutPlaybackOverlayCells(state, cols, 0, -1);
  const int textPxW =
      std::min(state.windowWidth, geometry.layout.width * geometry.cellWidth);
  const int textPxH =
      std::min(state.windowHeight, geometry.layout.height * geometry.cellHeight);
  geometry.leftPx = std::clamp(
      static_cast<int>(std::lround(state.windowWidth * 0.02)), 0,
      std::max(0, state.windowWidth - textPxW));
  geometry.topPx = std::clamp(
      static_cast<int>(std::lround(state.windowHeight * 0.95)) - textPxH, 0,
      std::max(0, state.windowHeight - textPxH));
  return geometry;
}
}  // namespace

PlaybackOverlayState buildPlaybackOverlayState(
    const PlaybackOverlayInputs& inputs) {
  PlaybackOverlayState state;
  state.windowTitle = inputs.windowTitle;
  state.audioOk = inputs.audioOk;
  state.audioSupports50HzToggle = inputs.audioSupports50HzToggle;
  state.radioEnabled = inputs.radioEnabled;
  state.hz50Enabled = inputs.hz50Enabled;
  state.canCycleAudioTracks = inputs.canCycleAudioTracks;
  state.activeAudioTrackLabel = inputs.activeAudioTrackLabel;
  state.hasSubtitles = inputs.hasSubtitles;
  state.subtitlesEnabled = inputs.subtitlesEnabled;
  state.activeSubtitleLabel =
      (inputs.subtitleManager && inputs.hasSubtitles)
          ? inputs.subtitleManager->activeTrackLabel()
          : "N/A";
  state.subtitleClockUs = inputs.subtitleClockUs;
  state.seekingOverlay = inputs.seekingOverlay;
  state.displaySec = inputs.displaySec;
  state.totalSec = inputs.totalSec;
  state.volPct = inputs.volPct;
  state.overlayVisible = inputs.overlayVisible;
  state.paused = inputs.paused;
  state.audioFinished = inputs.audioFinished;
  state.pictureInPictureAvailable = inputs.pictureInPictureAvailable;
  state.pictureInPictureActive = inputs.pictureInPictureActive;
  state.subtitleRenderError = inputs.subtitleRenderError;
  state.screenWidth = inputs.screenWidth;
  state.screenHeight = inputs.screenHeight;
  state.windowWidth = inputs.windowWidth;
  state.windowHeight = inputs.windowHeight;
  state.artTop = inputs.artTop;
  state.progressBarX = inputs.progressBarX;
  state.progressBarY = inputs.progressBarY;
  state.progressBarWidth = inputs.progressBarWidth;

  if (inputs.subtitleManager) {
    state.subtitleText = buildSubtitleText(*inputs.subtitleManager,
                                           inputs.subtitlesEnabled,
                                           inputs.seekingOverlay,
                                           inputs.subtitleClockUs,
                                           inputs.hasSubtitles);
    state.subtitleCues = collectSubtitleCues(*inputs.subtitleManager,
                                             inputs.subtitlesEnabled,
                                             inputs.seekingOverlay,
                                             inputs.subtitleClockUs,
                                             inputs.hasSubtitles);
    if (inputs.subtitlesEnabled && !inputs.seekingOverlay &&
        inputs.subtitleClockUs >= 0 && inputs.hasSubtitles) {
      if (const SubtitleTrack* activeTrack =
              inputs.subtitleManager->activeTrack()) {
        state.subtitleAssScript = activeTrack->assScript;
        state.subtitleAssFonts = activeTrack->assFonts;
      }
    }
  }

  return state;
}

std::vector<WindowUiState::SubtitleCue> collectSubtitleCues(
    const SubtitleManager& subtitleManager, bool subtitlesEnabled,
    bool seekingOverlay, int64_t clockUs, bool hasSubtitles) {
  std::vector<WindowUiState::SubtitleCue> out;
  if (!subtitlesEnabled || seekingOverlay || clockUs < 0 || !hasSubtitles) {
    return out;
  }
  const SubtitleTrack* activeTrack = subtitleManager.activeTrack();
  if (!activeTrack) {
    return out;
  }

  std::vector<const SubtitleCue*> active;
  activeTrack->cuesAt(clockUs, &active);
  if (active.empty()) return out;
  out.reserve(active.size());
  for (const SubtitleCue* cue : active) {
    if (!cue) continue;
    const bool hasRenderableAss = cue->assStyled && !cue->rawText.empty();
    if (cue->text.empty() && !hasRenderableAss) continue;
    WindowUiState::SubtitleCue item;
    item.text = cue->text;
    item.rawText = cue->rawText;
    item.textRuns.reserve(cue->textRuns.size());
    for (const SubtitleTextRun& run : cue->textRuns) {
      if (run.text.empty()) continue;
      WindowUiState::SubtitleCue::TextRun itemRun;
      itemRun.text = run.text;
      itemRun.hasPrimaryColor = run.hasPrimaryColor;
      itemRun.primaryColor = Color{run.primaryR, run.primaryG, run.primaryB};
      itemRun.primaryAlpha = run.primaryAlpha;
      itemRun.hasBackColor = run.hasBackColor;
      itemRun.backColor = Color{run.backR, run.backG, run.backB};
      itemRun.backAlpha = run.backAlpha;
      item.textRuns.push_back(std::move(itemRun));
    }
    item.sizeScale = std::clamp(cue->sizeScale, 0.40f, 3.0f);
    item.scaleX = std::clamp(cue->scaleX, 0.40f, 3.5f);
    item.scaleY = std::clamp(cue->scaleY, 0.40f, 3.5f);
    item.fontName = cue->fontName;
    item.bold = cue->bold;
    item.italic = cue->italic;
    item.underline = cue->underline;
    item.assStyled = cue->assStyled;
    item.hasPrimaryColor = cue->hasPrimaryColor;
    item.primaryColor = Color{cue->primaryR, cue->primaryG, cue->primaryB};
    item.primaryAlpha = cue->primaryAlpha;
    item.hasBackColor = cue->hasBackColor;
    item.backColor = Color{cue->backR, cue->backG, cue->backB};
    item.backAlpha = cue->backAlpha;
    item.startUs = cue->startUs;
    item.endUs = cue->endUs;
    item.alignment = cue->alignment;
    item.layer = cue->layer;
    item.hasPosition = cue->hasPosition;
    item.posX = cue->posXNorm;
    item.posY = cue->posYNorm;
    if (cue->hasMove) {
      item.hasPosition = true;
      const double elapsedMs =
          static_cast<double>(std::max<int64_t>(0, clockUs - cue->startUs)) /
          1000.0;
      double t = 0.0;
      if (cue->moveEndMs > cue->moveStartMs) {
        t = (elapsedMs - static_cast<double>(cue->moveStartMs)) /
            static_cast<double>(cue->moveEndMs - cue->moveStartMs);
      } else if (elapsedMs >= static_cast<double>(cue->moveStartMs)) {
        t = 1.0;
      }
      t = std::clamp(t, 0.0, 1.0);
      item.posX =
          static_cast<float>(cue->moveStartXNorm +
                             (cue->moveEndXNorm - cue->moveStartXNorm) * t);
      item.posY =
          static_cast<float>(cue->moveStartYNorm +
                             (cue->moveEndYNorm - cue->moveStartYNorm) * t);
    }
    item.marginVNorm = cue->marginVNorm;
    item.marginLNorm = cue->marginLNorm;
    item.marginRNorm = cue->marginRNorm;
    out.push_back(std::move(item));
  }

  std::stable_sort(out.begin(), out.end(),
                   [](const WindowUiState::SubtitleCue& a,
                      const WindowUiState::SubtitleCue& b) {
                     if (a.layer != b.layer) return a.layer < b.layer;
                     if (a.sizeScale != b.sizeScale)
                       return a.sizeScale > b.sizeScale;
                     return a.text < b.text;
                   });
  return out;
}

std::string buildSubtitleText(const SubtitleManager& subtitleManager,
                             bool subtitlesEnabled, bool seekingOverlay,
                             int64_t clockUs, bool hasSubtitles) {
  if (!subtitlesEnabled || seekingOverlay || clockUs < 0 || !hasSubtitles) {
    return {};
  }
  const SubtitleTrack* activeTrack = subtitleManager.activeTrack();
  if (!activeTrack) {
    return {};
  }
  std::vector<const SubtitleCue*> active;
  activeTrack->cuesAt(clockUs, &active);
  if (active.empty()) return {};

  std::stable_sort(active.begin(), active.end(),
                   [](const SubtitleCue* a, const SubtitleCue* b) {
                     if (!a || !b) return a < b;
                     if (a->layer != b->layer) return a->layer < b->layer;
                     if (a->sizeScale != b->sizeScale)
                       return a->sizeScale > b->sizeScale;
                     if (a->startUs != b->startUs) return a->startUs < b->startUs;
                     return a->text < b->text;
                   });

  std::string merged;
  for (const SubtitleCue* cue : active) {
    if (!cue || cue->text.empty()) continue;
    if (!merged.empty()) merged.push_back('\n');
    merged += cue->text;
  }
  return merged;
}

std::vector<OverlayControlSpec> buildOverlayControlSpecs(
    const PlaybackOverlayState& state, int hoverIndex) {
  std::vector<OverlayControlSpec> out;
  auto addSpec = [&](OverlayControlSpec spec) {
    out.push_back(std::move(spec));
  };

  OverlayControlSpec radio{};
  radio.id = OverlayControlId::Radio;
  radio.active = state.radioEnabled;
  auto radioLabels = makeLabels(radio.active ? "Radio: AM" : "Radio: Dry");
  radio.normalText = radioLabels.first;
  radio.hoverText = radioLabels.second;
  radio.width =
      std::max(countVisibleChars(radio.normalText),
               countVisibleChars(radio.hoverText));
  addSpec(std::move(radio));

  if (state.audioOk && state.audioSupports50HzToggle) {
    OverlayControlSpec hz50{};
    hz50.id = OverlayControlId::Hz50;
    hz50.active = state.hz50Enabled;
    auto hzLabels = makeLabels(hz50.active ? "50Hz: 50" : "50Hz: Auto");
    hz50.normalText = hzLabels.first;
    hz50.hoverText = hzLabels.second;
    hz50.width =
        std::max(countVisibleChars(hz50.normalText),
                 countVisibleChars(hz50.hoverText));
    addSpec(std::move(hz50));
  }

  OverlayControlSpec audioTrack{};
  audioTrack.id = OverlayControlId::AudioTrack;
  std::string audioLabel = "Audio: N/A";
  if (state.canCycleAudioTracks && state.audioOk) {
    std::string activeAudio = state.activeAudioTrackLabel;
    if (activeAudio.empty()) activeAudio = "N/A";
    if (utf8DisplayWidth(activeAudio) > 14) {
      activeAudio = utf8TakeDisplayWidth(activeAudio, 14);
    }
    audioLabel = "Audio: " + activeAudio;
  }
  auto audioLabels = makeLabels(audioLabel);
  audioTrack.normalText = audioLabels.first;
  audioTrack.hoverText = audioLabels.second;
  audioTrack.active = state.audioOk && state.canCycleAudioTracks;
  audioTrack.width =
      std::max(countVisibleChars(audioTrack.normalText),
               countVisibleChars(audioTrack.hoverText));
  addSpec(std::move(audioTrack));

  OverlayControlSpec subtitles{};
  subtitles.id = OverlayControlId::Subtitles;
  subtitles.active = state.hasSubtitles;
  std::string subtitleLabel = "Subs: N/A";
  if (state.hasSubtitles) {
    if (!state.subtitlesEnabled) {
      subtitleLabel = "Subs: Off";
    } else {
      std::string activeSubtitle = state.activeSubtitleLabel;
      if (activeSubtitle.empty()) activeSubtitle = "N/A";
      if (utf8DisplayWidth(activeSubtitle) > 14) {
        activeSubtitle = utf8TakeDisplayWidth(activeSubtitle, 14);
      }
      subtitleLabel = "Subs: " + activeSubtitle;
    }
  }
  auto subtitleLabels = makeLabels(subtitleLabel);
  subtitles.normalText = subtitleLabels.first;
  subtitles.hoverText = subtitleLabels.second;
  subtitles.width =
      std::max(countVisibleChars(subtitles.normalText),
               countVisibleChars(subtitles.hoverText));
  addSpec(std::move(subtitles));

  if (state.pictureInPictureAvailable) {
    OverlayControlSpec pip{};
    pip.id = OverlayControlId::PictureInPicture;
    pip.active = state.pictureInPictureActive;
    auto pipLabels =
        makeLabels(pip.active ? "PiP: On" : "PiP: Off");
    pip.normalText = pipLabels.first;
    pip.hoverText = pipLabels.second;
    pip.width =
        std::max(countVisibleChars(pip.normalText),
                 countVisibleChars(pip.hoverText));
    addSpec(std::move(pip));
  }

  for (size_t i = 0; i < out.size(); ++i) {
    auto& spec = out[i];
    bool hovered = static_cast<int>(i) == hoverIndex;
    spec.renderText = hovered ? spec.hoverText : spec.normalText;
    int textWidth = countVisibleChars(spec.renderText);
    if (textWidth < spec.width) {
      spec.renderText.append(static_cast<size_t>(spec.width - textWidth), ' ');
    } else if (textWidth > spec.width) {
      spec.renderText = utf8TakeDisplayWidth(spec.renderText, spec.width);
    }
  }
  return out;
}

OverlayCellLayout layoutOverlayCells(const OverlayCellLayoutInput& input) {
  OverlayCellLayout layout;
  layout.width = std::max(1, input.width);

  int controlLineCount = 0;
  std::vector<PendingOverlayCellControl> pending =
      wrapOverlayControls(input.controls, layout.width, &controlLineCount);
  const bool hasSuffix = !input.suffix.empty();
  const int contentInset = layout.width > 2 ? 1 : 0;
  const int progressWidth = std::max(1, layout.width - contentInset * 2);

  if (input.height > 0) {
    layout.height = input.height;
    layout.progressBarX = contentInset;
    layout.progressBarY = layout.height - 1;
    layout.progressBarWidth = progressWidth;
    if (hasSuffix) {
      layout.suffixText = fitLine(input.suffix, layout.width);
      layout.suffixY = layout.progressBarY - 1;
      layout.suffixX =
          std::max(0, layout.width - utf8DisplayWidth(layout.suffixText));
    }

    const int controlsBottom =
        (layout.suffixY >= 0 ? layout.suffixY : layout.progressBarY) - 1;
    const int controlsTop =
        pending.empty() ? controlsBottom
                        : controlsBottom - (controlLineCount - 1);
    for (const PendingOverlayCellControl& item : pending) {
      OverlayCellControlLayoutItem placed;
      placed.text = item.text;
      placed.controlIndex = item.controlIndex;
      placed.x = item.x;
      placed.y = controlsTop + item.line;
      placed.width = item.width;
      placed.active = item.active;
      placed.hovered = item.hovered;
      layout.controls.push_back(std::move(placed));
    }

    const int titleBaseY =
        pending.empty()
            ? ((layout.suffixY >= 0 ? layout.suffixY : layout.progressBarY) - 1)
            : (controlsTop - 1);
    layout.titleText = fitLine(input.title, layout.width);
    layout.titleX = 0;
    layout.titleY = titleBaseY;
  } else {
    layout.height = 1 + controlLineCount + (hasSuffix ? 1 : 0) + 1;
    layout.titleText = fitLine(input.title, layout.width);
    layout.titleX = 0;
    layout.titleY = 0;

    const int controlsTop = 1;
    for (const PendingOverlayCellControl& item : pending) {
      OverlayCellControlLayoutItem placed;
      placed.text = item.text;
      placed.controlIndex = item.controlIndex;
      placed.x = item.x;
      placed.y = controlsTop + item.line;
      placed.width = item.width;
      placed.active = item.active;
      placed.hovered = item.hovered;
      layout.controls.push_back(std::move(placed));
    }

    if (hasSuffix) {
      layout.suffixText = fitLine(input.suffix, layout.width);
      layout.suffixY = controlsTop + controlLineCount;
      layout.suffixX =
          std::max(0, layout.width - utf8DisplayWidth(layout.suffixText));
    }
    layout.progressBarX = contentInset;
    layout.progressBarY = layout.height - 1;
    layout.progressBarWidth = progressWidth;
  }

  layout.topY = layout.progressBarY;
  auto useTop = [&](int y) {
    if (y != -1) layout.topY = std::min(layout.topY, y);
  };
  useTop(layout.titleY);
  useTop(layout.suffixY);
  for (const auto& item : layout.controls) {
    useTop(item.y);
  }
  return layout;
}

OverlayCellLayout layoutPlaybackOverlayCells(
    const PlaybackOverlayState& state, int width, int height,
    int hoverIndex) {
  std::vector<OverlayControlSpec> specs =
      buildOverlayControlSpecs(state, hoverIndex);

  OverlayCellLayoutInput input;
  input.width = width;
  input.height = height;
  input.title = " " + buildWindowOverlayTopLine(state);
  input.suffix = buildWindowOverlayProgressSuffix(state);
  input.controls.reserve(specs.size());
  for (size_t i = 0; i < specs.size(); ++i) {
    OverlayCellControlInput control;
    control.text = specs[i].renderText;
    control.width = specs[i].width;
    control.active = specs[i].active;
    control.hovered = static_cast<int>(i) == hoverIndex;
    control.controlIndex = static_cast<int>(i);
    input.controls.push_back(std::move(control));
  }
  return layoutOverlayCells(input);
}

int overlayCellControlAt(const OverlayCellLayout& layout, int cellX,
                         int cellY) {
  for (const OverlayCellControlLayoutItem& item : layout.controls) {
    if (cellY != item.y) continue;
    if (cellX >= item.x && cellX < item.x + item.width) {
      return item.controlIndex;
    }
  }
  return -1;
}

std::string buildWindowOverlayProgressSuffix(
    const PlaybackOverlayState& state) {
  std::string status = "\xE2\x96\xB6";  // ▶
  if (state.audioFinished || state.paused) {
    status = state.audioFinished ? "\xE2\x96\xA0" : "\xE2\x8F\xB8";
  }
  auto formatTime = [](double s) -> std::string {
    if (!(s >= 0.0) || !std::isfinite(s)) return "--:--";
    int total = static_cast<int>(std::llround(s));
    int h = total / 3600;
    int m = (total % 3600) / 60;
    int sec = total % 60;
    char buf[64];
    if (h > 0)
      std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, sec);
    else
      std::snprintf(buf, sizeof(buf), "%02d:%02d", m, sec);
    return std::string(buf);
  };
  std::string timeLabel = state.totalSec > 0.0
                              ? (formatTime(state.displaySec) + " / " +
                                 formatTime(state.totalSec))
                              : formatTime(state.displaySec);
  std::string volStr = " Vol: " + std::to_string(state.volPct) + "%";
  return timeLabel + " " + status + volStr;
}

std::string buildWindowOverlayTopLine(const PlaybackOverlayState& state) {
  return state.windowTitle;
}

bool isBackMousePressed(const MouseEvent& mouse) {
  const DWORD backMask = RIGHTMOST_BUTTON_PRESSED |
                         FROM_LEFT_2ND_BUTTON_PRESSED |
                         FROM_LEFT_3RD_BUTTON_PRESSED |
                         FROM_LEFT_4TH_BUTTON_PRESSED;
  return (mouse.buttonState & backMask) != 0;
}

static bool terminalOverlayProgressRatioAt(const PlaybackOverlayState& state,
                                           const MouseEvent& mouse,
                                           double* outRatio) {
  if (!state.overlayVisible) return false;
  OverlayCellLayout layout = layoutPlaybackOverlayCells(
      state, state.screenWidth, state.screenHeight, -1);
  if (mouse.pos.Y != layout.progressBarY || layout.progressBarWidth <= 0) {
    return false;
  }
  const int rel = mouse.pos.X - layout.progressBarX;
  if (rel < 0 || rel >= layout.progressBarWidth) {
    return false;
  }
  if (outRatio) {
    *outRatio = std::clamp(
        static_cast<double>(rel) /
            static_cast<double>(std::max(1, layout.progressBarWidth - 1)),
        0.0, 1.0);
  }
  return true;
}

static bool windowOverlayProgressRatioAt(const PlaybackOverlayState& state,
                                         const MouseEvent& mouse,
                                         int cellPixelWidth,
                                         int cellPixelHeight,
                                         double* outRatio) {
  if (!state.overlayVisible) return false;
  WindowOverlayCellGeometry geometry = buildWindowOverlayCellGeometry(
      state, cellPixelWidth, cellPixelHeight);
  if (geometry.layout.progressBarWidth <= 0) return false;

  const int localX = mouse.pos.X - geometry.leftPx;
  const int localY = mouse.pos.Y - geometry.topPx;
  if (localX < 0 || localY < 0) return false;
  const int cellX = localX / geometry.cellWidth;
  const int cellY = localY / geometry.cellHeight;
  if (cellY != geometry.layout.progressBarY) return false;
  const int rel = cellX - geometry.layout.progressBarX;
  if (rel < 0 || rel >= geometry.layout.progressBarWidth) return false;
  if (outRatio) {
    *outRatio = std::clamp(
        static_cast<double>(rel) /
            static_cast<double>(
                std::max(1, geometry.layout.progressBarWidth - 1)),
        0.0, 1.0);
  }
  return true;
}

bool overlayProgressRatioAt(const PlaybackOverlayState& state,
                            const MouseEvent& mouse, int cellPixelWidth,
                            int cellPixelHeight, double* outRatio) {
  const bool windowEvent = (mouse.control & 0x80000000) != 0;
  return windowEvent ? windowOverlayProgressRatioAt(
                           state, mouse, cellPixelWidth, cellPixelHeight,
                           outRatio)
                     : terminalOverlayProgressRatioAt(state, mouse, outRatio);
}

int terminalOverlayControlAt(const PlaybackOverlayState& state,
                            const MouseEvent& mouse) {
  if (!state.overlayVisible) return -1;
  OverlayCellLayout layout = layoutPlaybackOverlayCells(
      state, state.screenWidth, state.screenHeight, -1);
  return overlayCellControlAt(layout, mouse.pos.X, mouse.pos.Y);
}

int windowOverlayControlAt(const PlaybackOverlayState& state,
                          const MouseEvent& mouse, int cellPixelWidth,
                          int cellPixelHeight) {
  if (!state.overlayVisible) return -1;
  WindowOverlayCellGeometry geometry = buildWindowOverlayCellGeometry(
      state, cellPixelWidth, cellPixelHeight);
  const int localX = mouse.pos.X - geometry.leftPx;
  const int localY = mouse.pos.Y - geometry.topPx;
  if (localX < 0 || localY < 0) return -1;
  return overlayCellControlAt(geometry.layout, localX / geometry.cellWidth,
                              localY / geometry.cellHeight);
}

WindowUiState buildWindowUiState(const PlaybackOverlayState& state,
                                int hoverIndex) {
  WindowUiState ui;
  ui.progress =
      (state.totalSec > 0.0 && std::isfinite(state.totalSec))
          ? static_cast<float>(std::clamp(state.displaySec / state.totalSec, 0.0, 1.0))
          : 0.0f;
  ui.overlayAlpha = state.overlayVisible ? 1.0f : 0.0f;
  ui.isPaused = state.paused;
  ui.title = state.windowTitle;
  std::vector<OverlayControlSpec> controlSpecs =
      buildOverlayControlSpecs(state, hoverIndex);
  ui.progressSuffix = buildWindowOverlayProgressSuffix(state);
  ui.controlButtons.clear();
  ui.controlButtons.reserve(controlSpecs.size());
  for (size_t i = 0; i < controlSpecs.size(); ++i) {
    WindowUiState::ControlButton btn;
    btn.text = controlSpecs[i].renderText;
    btn.active = controlSpecs[i].active;
    btn.hovered = static_cast<int>(i) == hoverIndex;
    ui.controlButtons.push_back(std::move(btn));
  }
  ui.subtitleClockUs = state.subtitleClockUs;
  ui.subtitleAssScript = state.subtitleAssScript;
  ui.subtitleAssFonts = state.subtitleAssFonts;
  ui.subtitleRenderError = state.subtitleRenderError;
  ui.subtitleCues = state.subtitleCues;
  ui.displaySec = state.displaySec;
  ui.totalSec = state.totalSec;
  ui.volPct = state.volPct;
  ui.subtitle = state.subtitleText;
  ui.subtitleAlpha =
      (state.subtitleCues.empty() && !state.subtitleAssScript) ? 0.0f : 1.0f;
  return ui;
}

}  // namespace playback_overlay
