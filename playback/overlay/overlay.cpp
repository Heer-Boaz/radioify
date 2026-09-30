#include "overlay.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>

#include "core/utf8.h"
#include "playback/video/edit/overlay_model.h"
#include "playback/video/edit/suggestion_panel.h"
#include "playback/video/image.h"
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
    control.tone = ui.controlButtons[i].tone;
    input.controls.push_back(std::move(control));
  }
  return layoutOverlayCells(input);
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


template <typename Target>
void renderVideoEditTimelineToTarget(
    Target &target, const OverlayCellLayout &layout,
    const OverlayRenderStyles &styles, double progress,
    const playback_video_edit::EditSnapshot &edit,
    const playback_video_edit::ExportProgress *editExport,
    playback_video_edit::Prompt editPrompt,
    const std::optional<MediaActionConfirmationDialog>
        &mediaActionConfirmationPrompt) {
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
  const Style suggestionBoundaryStyle{{136, 118, 170}, styles.progressEmptyStyle.bg};
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
    case playback_video_edit::SceneSuggestionCellKind::Review:
      glyph = L'┄';
      style = &dialogueStyle;
      break;
    case playback_video_edit::SceneSuggestionCellKind::Keep:
      glyph = L'━';
      style = &cutsceneStyle;
      break;
    case playback_video_edit::SceneSuggestionCellKind::Shorten:
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
                     L'┊', suggestionBoundaryStyle);
  }


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
  for (const auto &line :
       layoutTransientMessageCells(message, target.width(), target.height()))
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
void renderEditSuggestionsToTarget(Target& target, const OverlayCellLayout& layout,
                                   const OverlayRenderStyles& styles,
                                   const playback_video_edit::EditSnapshot& edit) {
  const auto panel = playback_video_edit::layoutSuggestionPanel(
      edit, target.width(), target.height(), layout.topY);
  if (!panel.drawable()) return;
  target.clearRect(panel.x, panel.y, panel.width, panel.height, styles.baseStyle);
  const int right = panel.x + panel.width - 1;
  const int bottom = panel.y + panel.height - 1;
  for (int x = panel.x + 1; x < right; ++x) {
    target.writeChar(x, panel.y, L'─', styles.accentStyle);
    target.writeChar(x, bottom, L'─', styles.accentStyle);
  }
  for (int y = panel.y + 1; y < bottom; ++y) {
    target.writeChar(panel.x, y, L'│', styles.accentStyle);
    target.writeChar(right, y, L'│', styles.accentStyle);
  }
  target.writeChar(panel.x, panel.y, L'┌', styles.accentStyle);
  target.writeChar(right, panel.y, L'┐', styles.accentStyle);
  target.writeChar(panel.x, bottom, L'└', styles.accentStyle);
  target.writeChar(right, bottom, L'┘', styles.accentStyle);
  if (panel.scrollOffset > 0)
    target.writeChar(right, panel.y + 1, L'↑', styles.accentStyle);
  if (panel.scrollOffset < panel.maximumScrollOffset)
    target.writeChar(right, bottom - 1, L'↓', styles.accentStyle);
  for (size_t index = 0; index < panel.lines.size(); ++index) {
    const auto& line = panel.lines[index];
    const int y = panel.y + 1 + static_cast<int>(index);
    Style style = line.accent ? styles.accentStyle : styles.baseStyle;
    if (line.selected) {
      style = {styles.baseStyle.bg, styles.accentStyle.fg};
      target.clearRect(panel.x + 1, y, panel.width - 2, 1, style);
    }
    const int width = index == 0 ? panel.closeColumn - 3 : panel.width - 4;
    target.writeText(panel.x + 2, y, utf8TakeDisplayWidth(line.text, width), style);
  }
  target.writeText(panel.x + panel.closeColumn, panel.y + 1, "[Close]", styles.accentStyle);
}

template <typename Target>
void renderOverlayToTarget(
    Target &target, const OverlayCellLayout &layout,
    const OverlayRenderStyles &styles, double progress,
    const playback_video_edit::EditSnapshot *videoEdit,
    const playback_video_edit::ExportProgress *videoEditExport,
    playback_video_edit::Prompt videoEditPrompt,
    const std::optional<MediaActionConfirmationDialog>
        &mediaActionConfirmationPrompt) {
  if (!target.isDrawable())
    return;

  const bool modalDialog = layout.dialog && layout.dialog->valid();
  if (modalDialog) {
    renderDialogToTarget(target, *layout.dialog, styles);
  }

  for (const auto &item : layout.controls) {
    Style style = Style{lerpColor(styles.baseStyle.fg, styles.baseStyle.bg,
                                  0.55f),
                        styles.baseStyle.bg};
    if (item.enabled) {
      if (item.tone == OverlayControlTone::Error)
        style = styles.errorStyle;
      else if (item.tone == OverlayControlTone::Warning)
        style = styles.warningStyle;
      else
        style = item.active ? styles.accentStyle : styles.baseStyle;
    }
    if (item.enabled && item.hovered) {
      style = {style.bg, style.fg};
    }
    target.writeControlText(item.text, item.y, item.x, item.width, style);
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
        videoEditPrompt, mediaActionConfirmationPrompt);
  }

  target.writeText(layout.suffixX, layout.suffixY, layout.suffixText,
                   styles.baseStyle);

  if (videoEdit && videoEdit->active && videoEdit->suggestionReview.visible) {
    if (videoEditPrompt == playback_video_edit::Prompt::None && !mediaActionConfirmationPrompt)
      renderEditSuggestionsToTarget(target, layout, styles, *videoEdit);
  }
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
    const auto& line = layout.metadataLines[static_cast<size_t>(index)];
    const std::string text = utf8TakeDisplayWidth(
        line.text,
        layout.metadataWidth);
    target.writeText(layout.metadataX, layout.metadataY + index, text,
                     line.role == playback_video_timeline_preview::MetadataRole::Title
                         ? styles.accentStyle : styles.baseStyle);
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
        &mediaActionConfirmationPrompt, int minY, int maxY) {
  ScreenOverlayTarget target(screen, minY, maxY);
  renderOverlayToTarget(target, layout, styles, progress, videoEdit,
                        videoEditExport, videoEditPrompt,
                        mediaActionConfirmationPrompt);
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
                          ui.videoEditPrompt, ui.mediaActionConfirmationPrompt);
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
