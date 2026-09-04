#include "overlay.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "core/utf8.h"
#include "playback/media_processing_presentation.h"
#include "playback/video/chapter/presentation.h"
#include "playback/video/edit/overlay_model.h"
#include "playback/video/image.h"
#include "subtitle_effects.h"
#include "ui_helpers.h"

namespace playback_overlay {
namespace {

std::string fitCellText(const std::string &text, int width) {
  if (width <= 0)
    return {};
  std::string filtered;
  filtered.reserve(text.size());
  for (char c : text) {
    if (c == '\r' || c == '\n')
      continue;
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

std::string
overlayTitleWithDebugLines(const std::vector<std::string> &debugLines,
                           const std::string &title) {
  std::string out;
  for (const std::string &line : debugLines) {
    if (line.empty())
      continue;
    if (!out.empty())
      out.push_back('\n');
    out += line;
  }
  if (!out.empty())
    out.push_back('\n');
  out += " " + title;
  return out;
}

uint32_t overlayGpuRgb(const Color &color) {
  return gpuTextGridRgb(color.r, color.g, color.b);
}

GpuTextGridCell overlayGpuCell(wchar_t ch, const Style &style,
                               uint32_t flags = 0) {
  return GpuTextGridCell{playback_gpu_text_grid::glyphIndex(
                             static_cast<uint32_t>(ch)),
                         overlayGpuRgb(style.fg),
                         overlayGpuRgb(style.bg), flags};
}

std::wstring overlayUtf8ToWide(const std::string &text) {
  std::wstring out = utf8ToWideLossy(text);
  out.erase(
      std::remove_if(out.begin(), out.end(),
                     [](wchar_t ch) { return ch == L'\r' || ch == L'\n'; }),
      out.end());
  return out;
}

} // namespace

OverlayCellLayout layoutMediaActionConfirmationDialogCells(
    const MediaActionConfirmationDialog &prompt, int width, int height,
    int hoverControlToken) {
  OverlayDialogLayoutInput input;
  input.width = width;
  input.height = height;
  input.title = prompt.title;
  input.text = prompt.text;
  input.buttons =
      buildMediaActionConfirmationDialogButtons(prompt, hoverControlToken);
  return layoutOverlayDialogCells(input);
}

PlaybackOverlayState
buildPlaybackOverlayState(const PlaybackOverlayInputs &inputs) {
  PlaybackOverlayState state;
  state.windowTitle = inputs.windowTitle;
  state.audioOk = inputs.audioOk;
  state.playPauseAvailable = inputs.playPauseAvailable;
  state.audioSupports50HzToggle = inputs.audioSupports50HzToggle;
  state.canPlayPrevious = inputs.canPlayPrevious;
  state.canPlayNext = inputs.canPlayNext;
  state.radioEnabled = inputs.radioEnabled;
  state.radioLabel = inputs.radioLabel;
  state.hz50Enabled = inputs.hz50Enabled;
  state.canCycleAudioTracks = inputs.canCycleAudioTracks;
  state.activeAudioTrackLabel = inputs.activeAudioTrackLabel;
  state.hasSubtitles = inputs.hasSubtitles;
  state.subtitlesEnabled = inputs.subtitlesEnabled;
  state.activeSubtitleLabel = inputs.subtitle.activeTrackLabel;
  state.subtitleClockUs = inputs.subtitleClockUs;
  state.seekingOverlay = inputs.seekingOverlay;
  state.displaySec = inputs.displaySec;
  state.totalSec = inputs.totalSec;
  state.volPct = inputs.volPct;
  state.overlayVisible = inputs.osd.controlsVisible;
  state.transientMessage = inputs.osd.message;
  state.paused = inputs.paused;
  state.pictureInPictureAvailable = inputs.pictureInPictureAvailable;
  state.pictureInPictureActive = inputs.pictureInPictureActive;
  state.subtitleText = inputs.subtitle.text;
  state.subtitleAssScript = inputs.subtitle.assScript;
  state.subtitleAssFonts = inputs.subtitle.assFonts;
  state.subtitleCues = inputs.subtitle.cues;
  state.subtitleRenderError = inputs.subtitleRenderError;
  state.debugLines = inputs.debugLines;
  state.contextMenu = inputs.contextMenu;
  state.videoEdit = inputs.videoEdit;
  state.videoEditExport = inputs.videoEditExport;
  state.videoEditPrompt = inputs.videoEditPrompt;
  state.mediaActionConfirmationPrompt = inputs.mediaActionConfirmationPrompt;
  state.mediaTaskActivity = inputs.mediaTaskActivity;
  state.chapters = inputs.chapters;
  state.chapterControlVisible = inputs.chapterControlVisible;
  state.chapterActivityPhase =
      std::clamp(inputs.chapterActivityPhase, 0.0, 1.0);
  state.chapterActivityMotionEnabled = inputs.chapterActivityMotionEnabled;
  state.chapterOverviewOpen = inputs.chapterOverviewOpen;
  state.chapterOverviewScrollOffset = inputs.chapterOverviewScrollOffset;
  state.chromeVisible =
      state.overlayVisible || !state.debugLines.empty() ||
      state.mediaActionConfirmationPrompt.has_value() ||
      state.mediaTaskActivity.has_value() || state.chapterOverviewOpen ||
      playback_video_edit::needsOverlayPresentation(
          state.videoEdit, state.videoEditExport, state.videoEditPrompt);

  return state;
}

SubtitlePresentation
projectSubtitlePresentation(const SubtitleManager &subtitleManager,
                            bool subtitlesEnabled, bool seekingOverlay,
                            int64_t clockUs, bool hasSubtitles) {
  SubtitlePresentation presentation;
  presentation.activeTrackLabel =
      hasSubtitles ? subtitleManager.activeTrackLabel() : "N/A";
  presentation.text = buildSubtitleText(subtitleManager, subtitlesEnabled,
                                        seekingOverlay, clockUs, hasSubtitles);
  presentation.cues = collectSubtitleCues(
      subtitleManager, subtitlesEnabled, seekingOverlay, clockUs, hasSubtitles);
  if (subtitlesEnabled && !seekingOverlay && clockUs >= 0 && hasSubtitles) {
    if (const SubtitleTrack *activeTrack = subtitleManager.activeTrack()) {
      presentation.assScript = activeTrack->assScript;
      presentation.assFonts = activeTrack->assFonts;
    }
  }
  return presentation;
}

std::vector<WindowUiState::SubtitleCue>
collectSubtitleCues(const SubtitleManager &subtitleManager,
                    bool subtitlesEnabled, bool seekingOverlay, int64_t clockUs,
                    bool hasSubtitles) {
  std::vector<WindowUiState::SubtitleCue> out;
  if (!subtitlesEnabled || seekingOverlay || clockUs < 0 || !hasSubtitles) {
    return out;
  }
  const SubtitleTrack *activeTrack = subtitleManager.activeTrack();
  if (!activeTrack) {
    return out;
  }

  std::vector<const SubtitleCue *> active;
  activeTrack->cuesAt(clockUs, &active);
  if (active.empty())
    return out;
  out.reserve(active.size());
  for (const SubtitleCue *cue : active) {
    if (!cue)
      continue;
    const float fadeOpacity = subtitleFadeOpacity(*cue, clockUs);
    if (fadeOpacity <= 0.001f)
      continue;
    const bool hasRenderableAss = cue->assStyled && !cue->rawText.empty();
    if (cue->text.empty() && !hasRenderableAss)
      continue;
    WindowUiState::SubtitleCue item;
    item.text = cue->text;
    item.rawText = cue->rawText;
    item.textRuns.reserve(cue->textRuns.size());
    for (const SubtitleTextRun &run : cue->textRuns) {
      if (run.text.empty())
        continue;
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
    item.hasClip = cue->hasClip;
    item.inverseClip = cue->inverseClip;
    item.clipX1 = cue->clipX1Norm;
    item.clipY1 = cue->clipY1Norm;
    item.clipX2 = cue->clipX2Norm;
    item.clipY2 = cue->clipY2Norm;
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
      item.posX = static_cast<float>(
          cue->moveStartXNorm + (cue->moveEndXNorm - cue->moveStartXNorm) * t);
      item.posY = static_cast<float>(
          cue->moveStartYNorm + (cue->moveEndYNorm - cue->moveStartYNorm) * t);
    }
    item.marginVNorm = cue->marginVNorm;
    item.marginLNorm = cue->marginLNorm;
    item.marginRNorm = cue->marginRNorm;
    applySubtitleTimelineEffects(&item, *cue, clockUs, fadeOpacity);
    out.push_back(std::move(item));
  }

  std::stable_sort(out.begin(), out.end(),
                   [](const WindowUiState::SubtitleCue &a,
                      const WindowUiState::SubtitleCue &b) {
                     if (a.layer != b.layer)
                       return a.layer < b.layer;
                     if (a.sizeScale != b.sizeScale)
                       return a.sizeScale > b.sizeScale;
                     return a.text < b.text;
                   });
  return out;
}

std::string buildSubtitleText(const SubtitleManager &subtitleManager,
                              bool subtitlesEnabled, bool seekingOverlay,
                              int64_t clockUs, bool hasSubtitles) {
  if (!subtitlesEnabled || seekingOverlay || clockUs < 0 || !hasSubtitles) {
    return {};
  }
  const SubtitleTrack *activeTrack = subtitleManager.activeTrack();
  if (!activeTrack) {
    return {};
  }
  std::vector<const SubtitleCue *> active;
  activeTrack->cuesAt(clockUs, &active);
  if (active.empty())
    return {};

  std::stable_sort(active.begin(), active.end(),
                   [](const SubtitleCue *a, const SubtitleCue *b) {
                     if (!a || !b)
                       return a < b;
                     if (a->layer != b->layer)
                       return a->layer < b->layer;
                     if (a->sizeScale != b->sizeScale)
                       return a->sizeScale > b->sizeScale;
                     if (a->startUs != b->startUs)
                       return a->startUs < b->startUs;
                     return a->text < b->text;
                   });

  std::string merged;
  for (const SubtitleCue *cue : active) {
    if (!cue || cue->text.empty())
      continue;
    if (!merged.empty())
      merged.push_back('\n');
    merged += cue->text;
  }
  return merged;
}

OverlayCellLayout layoutPlaybackOverlayCells(const PlaybackOverlayState &state,
                                             int width, int height,
                                             int hoverControlToken) {
  if (state.mediaActionConfirmationPrompt) {
    return layoutMediaActionConfirmationDialogCells(
        *state.mediaActionConfirmationPrompt, width, height, hoverControlToken);
  }
  std::vector<OverlayControlSpec> specs =
      buildOverlayControlSpecs(state, hoverControlToken);

  OverlayCellLayoutInput input;
  input.width = width;
  input.height = height;
  input.title = overlayTitleWithDebugLines(state.debugLines,
                                           buildWindowOverlayTopLine(state));
  input.suffix = buildWindowOverlayProgressSuffix(state);
  input.reservedRowsAboveProgress =
      (state.mediaActionConfirmationPrompt ||
               playback_video_edit::needsOverlayPresentation(
                   state.videoEdit, state.videoEditExport,
                   state.videoEditPrompt)
           ? 1
           : 0);
  input.controls = buildOverlayCellControlInputs(specs, hoverControlToken);
  return layoutOverlayCells(input);
}

OverlayCellLayout layoutWindowOverlayCells(const WindowUiState &ui, int width,
                                           int height) {
  if (ui.mediaActionConfirmationPrompt) {
    int hoverControlToken = -1;
    for (const WindowUiState::ControlButton &control : ui.controlButtons) {
      if (control.hovered) {
        hoverControlToken = overlayControlToken(control.id);
        break;
      }
    }
    return layoutMediaActionConfirmationDialogCells(
        *ui.mediaActionConfirmationPrompt, width, height, hoverControlToken);
  }
  OverlayCellLayoutInput input;
  input.width = width;
  input.height = height;
  input.title = overlayTitleWithDebugLines(ui.debugLines, ui.title);
  input.suffix = ui.progressSuffix;
  input.reservedRowsAboveProgress =
      (ui.mediaActionConfirmationPrompt ||
               playback_video_edit::needsOverlayPresentation(
                   ui.videoEdit, ui.videoEditExport, ui.videoEditPrompt)
           ? 1
           : 0);
  input.controls.reserve(ui.controlButtons.size());
  for (size_t i = 0; i < ui.controlButtons.size(); ++i) {
    OverlayCellControlInput control;
    control.id = ui.controlButtons[i].id;
    control.text = ui.controlButtons[i].text;
    control.active = ui.controlButtons[i].active;
    control.hovered = ui.controlButtons[i].hovered;
    control.enabled = ui.controlButtons[i].enabled;
    input.controls.push_back(std::move(control));
  }
  return layoutOverlayCells(input);
}

std::string buildWindowOverlayTopLine(const PlaybackOverlayState &state) {
  const std::string badge =
      playback_video_edit::retainedProgramBadge(state.videoEdit);
  const std::string playbackTitle =
      badge.empty() ? state.windowTitle : badge + " " + state.windowTitle;
  if (!state.mediaTaskActivity)
    return playbackTitle;
  const std::string task =
      playback_media_processing::activityStatusLine(*state.mediaTaskActivity);
  return task.empty() ? playbackTitle : task + "\n" + playbackTitle;
}

WindowUiState buildWindowUiState(const PlaybackOverlayState &state,
                                 int hoverControlToken) {
  WindowUiState ui;
  ui.progress = (state.totalSec > 0.0 && std::isfinite(state.totalSec))
                    ? static_cast<float>(std::clamp(
                          state.displaySec / state.totalSec, 0.0, 1.0))
                    : 0.0f;
  ui.overlayAlpha = state.overlayVisible ? 1.0f : 0.0f;
  ui.chromeVisible = state.chromeVisible;
  ui.isPaused = state.paused;
  ui.title = buildWindowOverlayTopLine(state);
  ui.transientMessage = state.transientMessage;
  std::vector<OverlayControlSpec> controlSpecs =
      buildOverlayControlSpecs(state, hoverControlToken);
  ui.progressSuffix = buildWindowOverlayProgressSuffix(state);
  ui.controlButtons.clear();
  ui.controlButtons.reserve(controlSpecs.size());
  for (size_t i = 0; i < controlSpecs.size(); ++i) {
    WindowUiState::ControlButton btn;
    btn.id = controlSpecs[i].id;
    btn.text = controlSpecs[i].renderText;
    btn.active = controlSpecs[i].active;
    btn.enabled = controlSpecs[i].enabled;
    btn.hovered = controlSpecs[i].enabled &&
                  overlayControlToken(controlSpecs[i].id) == hoverControlToken;
    ui.controlButtons.push_back(std::move(btn));
  }
  ui.subtitleClockUs = state.subtitleClockUs;
  ui.subtitleAssScript = state.subtitleAssScript;
  ui.subtitleAssFonts = state.subtitleAssFonts;
  ui.subtitleRenderError = state.subtitleRenderError;
  ui.subtitleCues = state.subtitleCues;
  ui.displaySec = state.displaySec;
  ui.volPct = state.volPct;
  ui.subtitle = state.subtitleText;
  ui.subtitleAlpha =
      (state.subtitleCues.empty() && !state.subtitleAssScript) ? 0.0f : 1.0f;
  ui.contextMenu = state.contextMenu;
  ui.videoEdit = state.videoEdit;
  ui.videoEditExport = state.videoEditExport;
  ui.videoEditPrompt = state.videoEditPrompt;
  ui.mediaActionConfirmationPrompt = state.mediaActionConfirmationPrompt;
  ui.chapters = state.chapters;
  ui.chapterActivityPhase = state.chapterActivityPhase;
  ui.chapterActivityMotionEnabled = state.chapterActivityMotionEnabled;
  ui.chapterOverviewOpen = state.chapterOverviewOpen;
  ui.chapterOverviewScrollOffset = state.chapterOverviewScrollOffset;
  return ui;
}

namespace {

class ScreenOverlayTarget {
public:
  ScreenOverlayTarget(ConsoleScreen &screen, int minY, int maxY)
      : screen_(screen), width_(screen.width()), firstY_(std::max(0, minY)),
        lastY_(std::min(screen.height(), maxY)) {}

  bool isDrawable() const { return width_ > 0 && firstY_ < lastY_; }
  int width() const { return width_; }
  int height() const { return lastY_; }

  bool rowVisible(int y) const { return y >= firstY_ && y < lastY_; }

  void writeText(int x, int y, const std::string &text, const Style &style) {
    if (!rowVisible(y) || text.empty() || x >= width_)
      return;
    const int drawX = std::max(0, x);
    const int available = width_ - drawX;
    if (available <= 0)
      return;
    std::string clipped =
        x < 0 ? utf8SliceDisplayWidth(text, -x, available) : text;
    if (utf8DisplayWidth(clipped) > available) {
      clipped = utf8TakeDisplayWidth(clipped, available);
    }
    screen_.writeText(drawX, y, clipped, style);
  }

  void writeControlText(const std::string &text, int y, int x, int width,
                        const Style &style) {
    if (width <= 0)
      return;
    writeText(x, y, text, style);
  }

  void writeChar(int x, int y, wchar_t ch, const Style &style) {
    if (!rowVisible(y) || x < 0 || x >= width_)
      return;
    screen_.writeChar(x, y, ch, style);
  }

  void clearRect(int x, int y, int width, int height, const Style &style) {
    const int left = std::clamp(x, 0, width_);
    const int top = std::clamp(y, firstY_, lastY_);
    const int right = std::clamp(x + std::max(0, width), 0, width_);
    const int bottom = std::clamp(y + std::max(0, height), firstY_, lastY_);
    for (int row = top; row < bottom; ++row) {
      for (int column = left; column < right; ++column) {
        screen_.writeChar(column, row, L' ', style);
      }
    }
  }

private:
  ConsoleScreen &screen_;
  int width_ = 0;
  int firstY_ = 0;
  int lastY_ = 0;
};

class GpuTextGridOverlayTarget {
public:
  GpuTextGridOverlayTarget(GpuTextGridFrame &frame, int cols, int rows,
                           const Style &baseStyle)
      : frame_(frame),
        transparentSpace_(overlayGpuCell(L' ', baseStyle,
                                         kGpuTextGridCellFlagTransparentBg)) {
    frame_.cols = std::max(1, cols);
    frame_.rows = std::max(1, rows);
    const size_t cellCount =
        static_cast<size_t>(frame_.cols) * static_cast<size_t>(frame_.rows);
    frame_.cells.assign(cellCount, transparentSpace_);
  }

  bool isDrawable() const { return frame_.cols > 0 && frame_.rows > 0; }
  int width() const { return frame_.cols; }
  int height() const { return frame_.rows; }

  bool rowVisible(int y) const { return y >= 0 && y < frame_.rows; }

  void writeText(int x, int y, const std::string &text, const Style &style) {
    if (!rowVisible(y) || text.empty() || x >= frame_.cols)
      return;
    const int drawX = std::max(0, x);
    const int available = frame_.cols - drawX;
    if (available <= 0)
      return;
    const std::string clipped = x < 0
                                    ? utf8SliceDisplayWidth(text, -x, available)
                                    : utf8TakeDisplayWidth(text, available);
    const std::wstring wide = overlayUtf8ToWide(clipped);
    int dstX = drawX;
    for (size_t i = 0; i < wide.size() && dstX < frame_.cols; ++i, ++dstX) {
      frame_.cells[static_cast<size_t>(y * frame_.cols + dstX)] =
          overlayGpuCell(wide[i], style);
    }
  }

  void writeControlText(const std::string &text, int y, int x, int width,
                        const Style &style) {
    if (!rowVisible(y) || width <= 0 || x >= frame_.cols)
      return;
    const int startX = std::max(0, x);
    const int endX = std::min(frame_.cols, x + width);
    if (startX >= endX)
      return;
    const std::string clipped =
        x < 0 ? utf8SliceDisplayWidth(text, -x, endX - startX)
              : utf8TakeDisplayWidth(text, endX - startX);
    const std::wstring wide = overlayUtf8ToWide(clipped);
    for (int dstX = startX; dstX < endX; ++dstX) {
      const int srcX = dstX - startX;
      const wchar_t ch = srcX >= 0 && srcX < static_cast<int>(wide.size())
                             ? wide[static_cast<size_t>(srcX)]
                             : L' ';
      frame_.cells[static_cast<size_t>(y * frame_.cols + dstX)] =
          overlayGpuCell(ch, style);
    }
  }

  void writeChar(int x, int y, wchar_t ch, const Style &style) {
    if (!rowVisible(y) || x < 0 || x >= frame_.cols)
      return;
    frame_.cells[static_cast<size_t>(y * frame_.cols + x)] =
        overlayGpuCell(ch, style);
  }

  void clearRect(int x, int y, int width, int height) {
    const int left = std::clamp(x, 0, frame_.cols);
    const int top = std::clamp(y, 0, frame_.rows);
    const int right = std::clamp(x + std::max(0, width), 0, frame_.cols);
    const int bottom = std::clamp(y + std::max(0, height), 0, frame_.rows);
    for (int row = top; row < bottom; ++row) {
      for (int column = left; column < right; ++column) {
        frame_.cells[static_cast<size_t>(row * frame_.cols + column)] =
            transparentSpace_;
      }
    }
  }

  void clearRect(int x, int y, int width, int height, const Style &style) {
    const int left = std::clamp(x, 0, frame_.cols);
    const int top = std::clamp(y, 0, frame_.rows);
    const int right = std::clamp(x + std::max(0, width), 0, frame_.cols);
    const int bottom = std::clamp(y + std::max(0, height), 0, frame_.rows);
    const GpuTextGridCell opaqueSpace = overlayGpuCell(L' ', style);
    for (int row = top; row < bottom; ++row) {
      for (int column = left; column < right; ++column) {
        frame_.cells[static_cast<size_t>(row * frame_.cols + column)] =
            opaqueSpace;
      }
    }
  }

private:
  GpuTextGridFrame &frame_;
  GpuTextGridCell transparentSpace_;
};

OverlayCellTextLine layoutTransientMessageLine(const std::string &message,
                                               int width, int height) {
  OverlayCellTextLine line;
  const int safeWidth = std::max(1, width);
  const int horizontalInset = safeWidth > 2 ? 1 : 0;
  const int availableWidth = std::max(1, safeWidth - horizontalInset * 2);
  line.text = " " + message + " ";
  if (utf8DisplayWidth(line.text) > availableWidth) {
    line.text = utf8TakeDisplayWidth(line.text, availableWidth);
  }
  line.x = std::max(horizontalInset,
                    safeWidth - horizontalInset - utf8DisplayWidth(line.text));
  line.y = std::min(1, std::max(0, height - 1));
  return line;
}

template <typename Target>
void renderChapterMarkersToTarget(
    Target &target, const OverlayCellLayout &layout,
    const OverlayRenderStyles &styles,
    const playback_video_chapters::Snapshot *chapters) {
  if (!chapters || !chapters->ready() || layout.progressBarY < 0 ||
      layout.progressBarWidth <= 0 || !target.rowVisible(layout.progressBarY)) {
    return;
  }
  const playback_video_chapters::MarkerProjection projection =
      playback_video_chapters::projectMarkers(*chapters,
                                               layout.progressBarWidth);
  const Style markerStyle{{177, 143, 255}, styles.progressEmptyStyle.bg};
  for (const int cell : projection.boundaryCells) {
    const bool collision =
        std::binary_search(projection.collisionCells.begin(),
                           projection.collisionCells.end(), cell);
    target.writeChar(layout.progressBarX + cell, layout.progressBarY,
                     collision ? L'╫' : L'┊', markerStyle);
  }
}

template <typename Target>
void renderVideoEditTimelineToTarget(
    Target &target, const OverlayCellLayout &layout,
    const OverlayRenderStyles &styles, double progress,
    const playback_video_edit::EditSnapshot &edit,
    const playback_video_edit::ExportProgress *editExport,
    playback_video_edit::Prompt editPrompt,
    const std::optional<MediaActionConfirmationDialog>
        &mediaActionConfirmationPrompt,
    const playback_video_chapters::Snapshot *chapters) {
  // This function composes timeline layers, not an editor-only surface.  The
  // edit model may legitimately be empty during ordinary playback while the
  // chapter layer still has content.
  if (layout.progressBarY < 0 || layout.progressBarWidth <= 0 ||
      !target.rowVisible(layout.progressBarY)) {
    return;
  }

  const int width = layout.progressBarWidth;
  const playback_video_edit::OverlayModel model =
      playback_video_edit::buildOverlayModel(edit, editExport, editPrompt,
                                             width, progress);
  const Style keptStyle{styles.progressStart, styles.progressEmptyStyle.bg};
  const Style alternateKeptStyle{styles.progressEnd,
                                 styles.progressEmptyStyle.bg};
  const Style selectedStyle{styles.accentStyle.bg, styles.accentStyle.fg};
  const Style cutStyle{{255, 145, 96}, styles.progressEmptyStyle.bg};
  const Style chapterStyle{{136, 118, 170}, styles.progressEmptyStyle.bg};
  const Style dialogueStyle{{93, 205, 226}, styles.progressEmptyStyle.bg};
  const Style cutsceneStyle{{205, 132, 255}, styles.progressEmptyStyle.bg};
  const Style menuStyle{{165, 173, 190}, styles.progressEmptyStyle.bg};
  const Style selectedSuggestionStyle{{255, 215, 96},
                                      styles.progressEmptyStyle.bg};
  const Style playheadStyle{styles.baseStyle.bg, styles.baseStyle.fg};

  for (int cell = 0; cell < static_cast<int>(model.cells.size()); ++cell) {
    const playback_video_edit::TimelineCellKind kind =
        model.cells[static_cast<size_t>(cell)];
    const bool selected =
        kind == playback_video_edit::TimelineCellKind::Selected;
    const bool alternate =
        kind == playback_video_edit::TimelineCellKind::KeptAlternate;
    const wchar_t glyph = selected ? L'=' : (alternate ? L'━' : L'─');
    const Style &style =
        selected ? selectedStyle : (alternate ? alternateKeptStyle : keptStyle);
    target.writeChar(layout.progressBarX + cell, layout.progressBarY, glyph,
                     style);
  }

  for (int cell = 0; cell < static_cast<int>(model.sceneSuggestionCells.size());
       ++cell) {
    const playback_video_edit::SceneSuggestionCellKind kind =
        model.sceneSuggestionCells[static_cast<size_t>(cell)];
    wchar_t glyph = L' ';
    const Style *style = nullptr;
    switch (kind) {
    case playback_video_edit::SceneSuggestionCellKind::Dialogue:
      glyph = L'┄';
      style = &dialogueStyle;
      break;
    case playback_video_edit::SceneSuggestionCellKind::Cutscene:
      glyph = L'━';
      style = &cutsceneStyle;
      break;
    case playback_video_edit::SceneSuggestionCellKind::MenuOrLoading:
      glyph = L'░';
      style = &menuStyle;
      break;
    case playback_video_edit::SceneSuggestionCellKind::Selected:
      glyph = L'═';
      style = &selectedSuggestionStyle;
      break;
    case playback_video_edit::SceneSuggestionCellKind::None:
      break;
    }
    if (style) {
      target.writeChar(layout.progressBarX + cell, layout.progressBarY, glyph,
                       *style);
    }
  }

  for (const int boundaryCell : model.sceneSuggestionBoundaryCells) {
    target.writeChar(layout.progressBarX + boundaryCell, layout.progressBarY,
                     L'┊', chapterStyle);
  }

  // Automatic chapter markers share the transport bar with the edit model,
  // but transport-critical edit handles retain visual precedence below.
  renderChapterMarkersToTarget(target, layout, styles, chapters);

  for (const int cutCell : model.cutCells) {
    target.writeChar(layout.progressBarX + cutCell, layout.progressBarY, L'|',
                     cutStyle);
  }
  for (const int cutCell : model.smoothCutCells) {
    target.writeChar(layout.progressBarX + cutCell, layout.progressBarY, L'~',
                     cutStyle);
  }

  if (!model.cells.empty()) {
    target.writeChar(layout.progressBarX + model.playheadCell,
                     layout.progressBarY, L'│', playheadStyle);
  }
  if (edit.active && model.inCell) {
    target.writeChar(layout.progressBarX + *model.inCell, layout.progressBarY,
                     L'[', styles.accentStyle);
  }
  if (edit.active && model.outCell) {
    target.writeChar(layout.progressBarX + *model.outCell, layout.progressBarY,
                     L']', styles.accentStyle);
  }

  const int statusY = layout.progressBarY - 1;
  if (!target.rowVisible(statusY))
    return;
  std::string status = model.status;
  if (mediaActionConfirmationPrompt) {
    status = mediaActionConfirmationPrompt->title;
  }
  status = utf8TakeDisplayWidth(status, target.width());
  target.writeText(0, statusY, status, styles.accentStyle);
}

template <typename Target>
void renderTransientMessageToTarget(Target &target, const std::string &message,
                                    const Style &style) {
  if (!target.isDrawable())
    return;
  const OverlayCellTextLine line =
      layoutTransientMessageLine(message, target.width(), target.height());
  target.writeText(line.x, line.y, line.text, style);
}

template <typename Target>
void renderContextMenuToTarget(Target &target,
                               const ContextMenuCellLayout &layout,
                               const OverlayRenderStyles &styles) {
  if (!target.isDrawable() || !layout.drawable())
    return;

  const int left = layout.x;
  const int right = layout.x + layout.width - 1;
  const int top = layout.y;
  const int bottom = layout.y + layout.height - 1;
  for (int x = left + 1; x < right; ++x) {
    target.writeChar(x, top, L'─', styles.accentStyle);
    target.writeChar(x, bottom, L'─', styles.accentStyle);
  }
  target.writeChar(left, top, L'┌', styles.accentStyle);
  target.writeChar(right, top, L'┐', styles.accentStyle);
  target.writeChar(left, bottom, L'└', styles.accentStyle);
  target.writeChar(right, bottom, L'┘', styles.accentStyle);

  const Style selectedStyle{styles.accentStyle.bg, styles.accentStyle.fg};
  for (const ContextMenuCellItem &item : layout.items) {
    const Style &rowStyle = item.selected ? selectedStyle : styles.baseStyle;
    const std::string row = fitCellText(" " + item.text, item.width);
    target.writeControlText(row, item.y, item.x, item.width, rowStyle);
    target.writeChar(left, item.y, L'│', styles.accentStyle);
    target.writeChar(right, item.y, L'│', styles.accentStyle);
  }
}

template <typename Target>
void renderDialogToTarget(Target &target, const OverlayCellDialogLayout &dialog,
                          const OverlayRenderStyles &styles) {
  if (!target.isDrawable() || !dialog.valid())
    return;

  const int left = dialog.x;
  const int right = dialog.x + dialog.width - 1;
  const int top = dialog.y;
  const int bottom = dialog.y + dialog.height - 1;
  for (int y = top; y <= bottom; ++y) {
    for (int x = left; x <= right; ++x) {
      target.writeChar(x, y, L' ', styles.baseStyle);
    }
  }
  for (int x = left + 1; x < right; ++x) {
    target.writeChar(x, top, L'-', styles.accentStyle);
    target.writeChar(x, bottom, L'-', styles.accentStyle);
  }
  for (int y = top + 1; y < bottom; ++y) {
    target.writeChar(left, y, L'|', styles.accentStyle);
    target.writeChar(right, y, L'|', styles.accentStyle);
  }
  target.writeChar(left, top, L'+', styles.accentStyle);
  target.writeChar(right, top, L'+', styles.accentStyle);
  target.writeChar(left, bottom, L'+', styles.accentStyle);
  target.writeChar(right, bottom, L'+', styles.accentStyle);

  target.writeText(dialog.titleX, dialog.titleY, dialog.title,
                   styles.accentStyle);
  for (const OverlayCellTextLine &line : dialog.contentLines) {
    target.writeText(line.x, line.y, line.text, styles.baseStyle);
  }
}

template <typename Target>
void renderChapterOverviewToTarget(
    Target &target, const OverlayCellLayout &overlayLayout,
    const OverlayRenderStyles &styles,
    const playback_video_chapters::Snapshot *chapters, bool chapterOverviewOpen,
    int chapterOverviewScrollOffset) {
  if (!chapters || !chapterOverviewOpen || !target.isDrawable())
    return;
  const playback_video_chapters::OverviewPanelLayout panel =
      playback_video_chapters::layoutOverviewPanel(
          *chapters, target.width(), target.height(),
          overlayLayout.topY, chapterOverviewScrollOffset);
  if (!panel.drawable())
    return;

  target.clearRect(panel.x, panel.y, panel.width, panel.height,
                   styles.baseStyle);
  const int left = panel.x;
  const int right = panel.x + panel.width - 1;
  const int top = panel.y;
  const int bottom = panel.y + panel.height - 1;
  for (int x = left + 1; x < right; ++x) {
    target.writeChar(x, top, L'─', styles.accentStyle);
    target.writeChar(x, bottom, L'─', styles.accentStyle);
  }
  for (int y = top + 1; y < bottom; ++y) {
    target.writeChar(left, y, L'│', styles.accentStyle);
    target.writeChar(right, y, L'│', styles.accentStyle);
  }
  target.writeChar(left, top, L'┌', styles.accentStyle);
  target.writeChar(right, top, L'┐', styles.accentStyle);
  target.writeChar(left, bottom, L'└', styles.accentStyle);
  target.writeChar(right, bottom, L'┘', styles.accentStyle);
  if (panel.scrollOffset > 0 && top + 1 < bottom) {
    target.writeChar(right, top + 1, L'↑', styles.accentStyle);
  }
  if (panel.scrollOffset < panel.maximumScrollOffset && bottom - 1 > top) {
    target.writeChar(right, bottom - 1, L'↓', styles.accentStyle);
  }

  const int contentWidth = std::max(1, panel.width - 4);
  const int lineCount =
      std::min<int>(panel.height - 2, static_cast<int>(panel.lines.size()));
  for (int index = 0; index < lineCount; ++index) {
    const auto &line = panel.lines[static_cast<std::size_t>(index)];
    for (const auto &run : line.runs) {
      if (run.column < 0 || run.column >= contentWidth)
        continue;
      target.writeText(
          panel.x + 2 + run.column, panel.y + 1 + index,
          utf8TakeDisplayWidth(run.text, contentWidth - run.column),
          run.role == playback_video_chapters::OverviewPanelLayout::TextRole::
                          Accent
              ? styles.accentStyle
              : styles.baseStyle);
    }
  }
}

template <typename Target>
void renderOverlayToTarget(
    Target &target, const OverlayCellLayout &layout,
    const OverlayRenderStyles &styles, double progress,
    const playback_video_edit::EditSnapshot *videoEdit,
    const playback_video_edit::ExportProgress *videoEditExport,
    playback_video_edit::Prompt videoEditPrompt,
    const std::optional<MediaActionConfirmationDialog>
        &mediaActionConfirmationPrompt,
    const playback_video_chapters::Snapshot *chapters,
    double chapterActivityPhase, bool chapterActivityMotionEnabled,
    bool chapterOverviewOpen,
    int chapterOverviewScrollOffset) {
  if (!target.isDrawable())
    return;

  const bool modalDialog = layout.dialog && layout.dialog->valid();
  if (modalDialog) {
    renderDialogToTarget(target, *layout.dialog, styles);
  }

  for (const auto &item : layout.controls) {
    Style style =
        item.enabled
            ? (item.active ? styles.accentStyle : styles.baseStyle)
            : Style{lerpColor(styles.baseStyle.fg, styles.baseStyle.bg, 0.55f),
                    styles.baseStyle.bg};
    if (item.enabled && item.hovered) {
      style = {style.bg, style.fg};
    }
    target.writeControlText(item.text, item.y, item.x, item.width, style);
    if (item.id == OverlayControlId::Chapters && !item.hovered && chapters) {
      // An indeterminate highlight travels through the existing affordance.
      // The violet tail reuses the chapter-marker color family; the warm head
      // uses the player's accent. Spaces remain untouched so the effect reads
      // as character illumination rather than a second progress bar.
      const std::vector<float> sweep = chapterControlCharacterHighlights(
          *chapters, chapterActivityMotionEnabled, item.width,
          chapterActivityPhase);
      const std::wstring glyphs = overlayUtf8ToWide(item.text);
      for (int column = 0;
           column < item.width && column < static_cast<int>(glyphs.size()) &&
           column < static_cast<int>(sweep.size());
           ++column) {
        const float intensity = sweep[static_cast<std::size_t>(column)];
        if (intensity <= 0.0f ||
            glyphs[static_cast<std::size_t>(column)] == L' ') {
          continue;
        }
        Style sweepStyle = style;
        sweepStyle.fg = intensity >= 1.0f
                            ? styles.accentStyle.fg
                            : lerpColor(style.fg, Color{177, 143, 255},
                                        intensity);
        target.writeChar(item.x + column, item.y,
                         glyphs[static_cast<std::size_t>(column)], sweepStyle);
      }
    }
  }

  if (modalDialog)
    return;

  for (const auto &titleLine : layout.titleLines) {
    target.writeText(titleLine.x, titleLine.y, titleLine.text,
                     styles.accentStyle);
  }

  if (layout.progressBarY >= 0 && layout.progressBarWidth > 0 &&
      target.rowVisible(layout.progressBarY)) {
    const int leftFrameX = layout.progressBarX - 1;
    const int rightFrameX = layout.progressBarX + layout.progressBarWidth;
    target.writeChar(leftFrameX, layout.progressBarY, L'|',
                     styles.progressFrameStyle);
    auto barCells = renderProgressBarCells(
        std::clamp(progress, 0.0, 1.0), layout.progressBarWidth,
        styles.progressEmptyStyle, styles.progressStart, styles.progressEnd);
    for (int i = 0; i < layout.progressBarWidth; ++i) {
      const auto &cell = barCells[static_cast<size_t>(i)];
      target.writeChar(layout.progressBarX + i, layout.progressBarY, cell.ch,
                       cell.style);
    }
    target.writeChar(rightFrameX, layout.progressBarY, L'|',
                     styles.progressFrameStyle);
  }

  if (videoEdit) {
    renderVideoEditTimelineToTarget(
        target, layout, styles, progress, *videoEdit, videoEditExport,
        videoEditPrompt, mediaActionConfirmationPrompt, chapters);
  } else {
    renderChapterMarkersToTarget(target, layout, styles, chapters);
  }

  target.writeText(layout.suffixX, layout.suffixY, layout.suffixText,
                   styles.baseStyle);

  renderChapterOverviewToTarget(target, layout, styles, chapters,
                                chapterOverviewOpen,
                                chapterOverviewScrollOffset);
}

template <typename Target>
void renderTimelinePreviewTimestampToTarget(
    Target &target, const playback_video_timeline_preview::CellLayout &layout,
    const OverlayRenderStyles &styles) {
  if (!target.isDrawable() || !layout.drawable())
    return;

  const int availableLabelWidth = std::max(0, layout.outerWidth - 4);
  const std::string label =
      utf8TakeDisplayWidth(layout.label, availableLabelWidth);
  const int labelWidth = utf8DisplayWidth(label);
  const int labelX =
      layout.outerX + std::max(2, (layout.outerWidth - labelWidth) / 2);
  target.writeText(labelX, layout.labelY, label, styles.accentStyle);
}

template <typename Target>
void renderTimelinePreviewMetadataToTarget(
    Target &target, const playback_video_timeline_preview::CellLayout &layout,
    const OverlayRenderStyles &styles) {
  if (!target.isDrawable() || !layout.drawable() || layout.metadataWidth <= 0 ||
      layout.metadataHeight <= 0) {
    return;
  }
  const int lineCount = std::min<int>(
      layout.metadataHeight, static_cast<int>(layout.metadataLines.size()));
  for (int index = 0; index < lineCount; ++index) {
    const std::string text = utf8TakeDisplayWidth(
        layout.metadataLines[static_cast<std::size_t>(index)],
        layout.metadataWidth);
    target.writeText(layout.metadataX, layout.metadataY + index, text,
                     index == 0 ? styles.accentStyle : styles.baseStyle);
  }
}

template <typename Target>
void renderTimelinePreviewChromeToTarget(
    Target &target, const playback_video_timeline_preview::CellLayout &layout,
    const OverlayRenderStyles &styles) {
  if (!target.isDrawable() || !layout.drawable())
    return;

  const int left = layout.outerX;
  const int right = layout.outerX + layout.outerWidth - 1;
  const int top = layout.outerY;
  const int bottom = layout.outerY + layout.outerHeight - 1;
  for (int x = left + 1; x < right; ++x) {
    target.writeChar(x, top, L'─', styles.accentStyle);
    target.writeChar(x, bottom, L'─', styles.accentStyle);
  }
  for (int y = top + 1; y < bottom; ++y) {
    target.writeChar(left, y, L'│', styles.accentStyle);
    target.writeChar(right, y, L'│', styles.accentStyle);
  }
  target.writeChar(left, top, L'┌', styles.accentStyle);
  target.writeChar(right, top, L'┐', styles.accentStyle);
  target.writeChar(left, bottom, L'└', styles.accentStyle);
  target.writeChar(right, bottom, L'┘', styles.accentStyle);

  renderTimelinePreviewTimestampToTarget(target, layout, styles);
  renderTimelinePreviewMetadataToTarget(target, layout, styles);
}

} // namespace

void renderOverlayToScreen(
    ConsoleScreen &screen, const OverlayCellLayout &layout,
    const OverlayRenderStyles &styles, double progress,
    const playback_video_edit::EditSnapshot *videoEdit,
    const playback_video_edit::ExportProgress *videoEditExport,
    playback_video_edit::Prompt videoEditPrompt,
    const std::optional<MediaActionConfirmationDialog>
        &mediaActionConfirmationPrompt,
    const playback_video_chapters::Snapshot *chapters,
    double chapterActivityPhase, bool chapterActivityMotionEnabled,
    bool chapterOverviewOpen,
    int chapterOverviewScrollOffset, int minY, int maxY) {
  ScreenOverlayTarget target(screen, minY, maxY);
  renderOverlayToTarget(target, layout, styles, progress, videoEdit,
                        videoEditExport, videoEditPrompt,
                        mediaActionConfirmationPrompt, chapters,
                        chapterActivityPhase, chapterActivityMotionEnabled,
                        chapterOverviewOpen,
                        chapterOverviewScrollOffset);
}

void renderTransientMessageToScreen(ConsoleScreen &screen,
                                    const std::string &message,
                                    const Style &style) {
  ScreenOverlayTarget target(screen, 0, screen.height());
  renderTransientMessageToTarget(target, message, style);
}

void renderContextMenuToScreen(ConsoleScreen &screen,
                               const ContextMenuCellLayout &layout,
                               const OverlayRenderStyles &styles) {
  ScreenOverlayTarget target(screen, 0, screen.height());
  renderContextMenuToTarget(target, layout, styles);
}

void renderTimelinePreviewChromeToScreen(
    ConsoleScreen &screen,
    const playback_video_timeline_preview::CellLayout &layout,
    const OverlayRenderStyles &styles) {
  ScreenOverlayTarget target(screen, 0, screen.height());
  renderTimelinePreviewChromeToTarget(target, layout, styles);
}

void renderTimelinePreviewTimestampToScreen(
    ConsoleScreen &screen,
    const playback_video_timeline_preview::CellLayout &layout,
    const OverlayRenderStyles &styles) {
  ScreenOverlayTarget target(screen, 0, screen.height());
  renderTimelinePreviewTimestampToTarget(target, layout, styles);
}

bool renderWindowUiToGpuTextGrid(
    const WindowUiState &ui, const OverlayCellLayout &overlayLayout,
    int cellPixelWidth, int cellPixelHeight,
    TimelinePreviewPresentation previewPresentation,
    const OverlayRenderStyles &styles, GpuTextGridFrame &outFrame) {
  GpuTextGridOverlayTarget target(outFrame, overlayLayout.width,
                                  overlayLayout.height, styles.baseStyle);
  bool rendered = false;
  if (ui.chromeVisible) {
    renderOverlayToTarget(target, overlayLayout, styles, ui.progress,
                          &ui.videoEdit, &ui.videoEditExport,
                          ui.videoEditPrompt, ui.mediaActionConfirmationPrompt,
                          &ui.chapters, ui.chapterActivityPhase,
                          ui.chapterActivityMotionEnabled,
                          ui.chapterOverviewOpen,
                          ui.chapterOverviewScrollOffset);
    rendered = true;
  }
  if (ui.timelinePreview.hoverActive) {
    const bool imageReady =
        previewPresentation == TimelinePreviewPresentation::ImageAndTimestamp &&
        ui.timelinePreview.hasImage();
    const playback_video_image::RgbaImage *previewSurface =
        imageReady ? &ui.timelinePreview.image->surface : nullptr;
    const int sourceWidth = previewSurface
                                ? static_cast<int>(previewSurface->width)
                                : std::max(16, ui.timelinePreview.sourceWidth);
    const int sourceHeight = previewSurface
                                 ? static_cast<int>(previewSurface->height)
                                 : std::max(9, ui.timelinePreview.sourceHeight);
    const auto previewLayout = playback_video_timeline_preview::layoutCells(
        overlayLayout.width, overlayLayout.height, overlayLayout.progressBarY,
        overlayLayout.progressBarX, overlayLayout.progressBarWidth,
        ui.timelinePreview.anchorRatio, sourceWidth, sourceHeight,
        cellPixelWidth, cellPixelHeight,
        playback_video_timeline_preview::formatTimestamp(
            ui.timelinePreview.targetUs),
        ui.timelinePreview.metadataLines);
    if (imageReady || !ui.timelinePreview.metadataLines.empty()) {
      target.clearRect(previewLayout.outerX, previewLayout.outerY,
                       previewLayout.outerWidth, previewLayout.outerHeight);
      renderTimelinePreviewChromeToTarget(target, previewLayout, styles);
    } else {
      renderTimelinePreviewTimestampToTarget(target, previewLayout, styles);
    }
    rendered = rendered || previewLayout.drawable();
  }
  if (ui.transientMessage) {
    renderTransientMessageToTarget(target, *ui.transientMessage,
                                   styles.accentStyle);
    rendered = true;
  }
  if (ui.contextMenu.visible) {
    const ContextMenuCellLayout menuLayout = layoutContextMenuCells(
        ui.contextMenu, overlayLayout.width, overlayLayout.height);
    renderContextMenuToTarget(target, menuLayout, styles);
    rendered = rendered || menuLayout.drawable();
  }
  return rendered;
}

} // namespace playback_overlay
