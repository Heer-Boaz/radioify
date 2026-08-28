#include "playback/overlay/overlay.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "ui/text_grid/dialog_layout.h"
#include "unicode_display_width.h"

namespace playback_overlay {
namespace {

std::string fitLayoutText(const std::string& text, int width) {
  if (width <= 0) return {};
  if (utf8DisplayWidth(text) <= width) return text;
  if (width == 1) return utf8TakeDisplayWidth(text, width);
  return utf8TakeDisplayWidth(text, width - 1) + "~";
}

std::vector<std::string> wrapLayoutText(const std::string& text, int width) {
  std::vector<std::string> lines;
  if (width <= 0) return lines;
  if (text.empty()) {
    lines.emplace_back();
    return lines;
  }

  size_t offset = 0;
  size_t lineStart = 0;
  size_t lineEnd = 0;
  int lineWidth = 0;
  while (offset < text.size()) {
    char32_t codepoint = 0;
    size_t startByte = 0;
    size_t endByte = 0;
    if (!utf8DecodeCodepoint(text, &offset, &codepoint, &startByte,
                             &endByte)) {
      break;
    }
    if (codepoint == U'\r') continue;
    if (codepoint == U'\n') {
      lines.emplace_back(text.substr(lineStart, lineEnd - lineStart));
      lineStart = offset;
      lineEnd = offset;
      lineWidth = 0;
      continue;
    }
    const int glyphWidth = unicodeDisplayWidth(codepoint);
    if (glyphWidth > 0 && lineWidth > 0 &&
        lineWidth + glyphWidth > width) {
      lines.emplace_back(text.substr(lineStart, lineEnd - lineStart));
      lineStart = startByte;
      lineEnd = startByte;
      lineWidth = 0;
    }
    lineEnd = endByte;
    lineWidth += glyphWidth;
  }
  lines.emplace_back(text.substr(lineStart, lineEnd - lineStart));
  return lines;
}

std::string fitControlText(const std::string& text, int width) {
  if (width <= 0) return {};
  std::string filtered;
  filtered.reserve(text.size());
  for (char c : text) {
    if (c != '\r' && c != '\n') filtered.push_back(c);
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

struct PendingControl {
  OverlayControlId id = OverlayControlId::Radio;
  std::string text;
  int x = 0;
  int line = 0;
  int width = 0;
  bool active = false;
  bool hovered = false;
  bool enabled = true;
};

std::vector<PendingControl> wrapControls(
    const std::vector<OverlayCellControlInput>& controls, int width,
    int* outLineCount) {
  const int contentInset = width > 2 ? 1 : 0;
  const int maxLineWidth = std::max(1, width - contentInset * 2);
  std::vector<PendingControl> out;
  out.reserve(controls.size());

  int cursor = 0;
  int line = 0;
  for (const OverlayCellControlInput& control : controls) {
    const int controlWidth = std::min(
        maxLineWidth,
        std::max(1, control.width > 0 ? control.width
                                      : utf8DisplayWidth(control.text)));
    const int gap = cursor > 0 ? 2 : 0;
    if (cursor > 0 && cursor + gap + controlWidth > maxLineWidth) {
      ++line;
      cursor = 0;
    } else {
      cursor += gap;
    }

    out.push_back(PendingControl{control.id,
                                 fitControlText(control.text, controlWidth),
                                 contentInset + cursor, line, controlWidth,
                                 control.active, control.hovered,
                                 control.enabled});
    cursor += controlWidth;
  }

  if (outLineCount) *outLineCount = out.empty() ? 0 : line + 1;
  return out;
}

OverlayCellControlLayoutItem placeControl(const PendingControl& item, int y) {
  return OverlayCellControlLayoutItem{item.id,    item.text,   item.x,      y,
                                      item.width, item.active, item.hovered,
                                      item.enabled};
}

}  // namespace

std::string buildWindowOverlayProgressSuffix(
    const PlaybackOverlayState& state) {
  // Edit mode owns the row above the timeline, including frame-accurate
  // timecode.  A modal prompt owns it exclusively.  Do not add a second,
  // rounded playback clock to either state.
  if (state.mediaTaskCancellationPrompt || state.videoEdit.active ||
      state.videoEditPrompt != playback_video_edit::Prompt::None) {
    return {};
  }

  const auto formatTime = [](double seconds) -> std::string {
    if (!(seconds >= 0.0) || !std::isfinite(seconds)) return "--:--";
    const int total = static_cast<int>(std::llround(seconds));
    const int hours = total / 3600;
    const int minutes = (total % 3600) / 60;
    const int remainingSeconds = total % 60;
    char buffer[64];
    if (hours > 0) {
      std::snprintf(buffer, sizeof(buffer), "%d:%02d:%02d", hours,
                    minutes, remainingSeconds);
    } else {
      std::snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes,
                    remainingSeconds);
    }
    return std::string(buffer);
  };

  std::string suffix =
      state.totalSec > 0.0
          ? formatTime(state.displaySec) + " / " + formatTime(state.totalSec)
          : formatTime(state.displaySec);
  if (state.audioOk) {
    suffix += " Vol: " + std::to_string(state.volPct) + "%";
  }
  return suffix;
}

OverlayCellLayout layoutOverlayCells(const OverlayCellLayoutInput& input) {
  OverlayCellLayout layout;
  layout.width = std::max(1, input.width);

  int controlLineCount = 0;
  const std::vector<PendingControl> pending =
      wrapControls(input.controls, layout.width, &controlLineCount);
  const bool hasSuffix = !input.suffix.empty();
  const int contentInset = layout.width > 2 ? 1 : 0;
  const int progressWidth = std::max(1, layout.width - contentInset * 2);
  const int reservedRowsAboveProgress =
      std::max(0, input.reservedRowsAboveProgress);
  std::vector<std::string> titleLines =
      wrapLayoutText(input.title, layout.width);
  if (titleLines.empty()) titleLines.emplace_back();
  auto placeTitleLines = [&](int topY, int firstLine, int lineCount) {
    layout.titleLines.clear();
    layout.titleX = 0;
    layout.titleY = -1;
    layout.titleText.clear();
    if (lineCount <= 0) return;
    layout.titleLines.reserve(static_cast<size_t>(lineCount));
    for (int i = 0; i < lineCount; ++i) {
      const int lineIndex = firstLine + i;
      layout.titleLines.push_back(OverlayCellTextLine{
          0, topY + i, titleLines[static_cast<size_t>(lineIndex)]});
    }
    layout.titleX = layout.titleLines.front().x;
    layout.titleY = layout.titleLines.front().y;
    layout.titleText = layout.titleLines.front().text;
  };

  if (input.height > 0) {
    layout.height = input.height;
    layout.progressBarX = contentInset;
    layout.progressBarY = layout.height - 1;
    layout.progressBarWidth = progressWidth;
    const int firstContentYAboveFooter =
        layout.progressBarY - reservedRowsAboveProgress;
    if (hasSuffix && firstContentYAboveFooter > 0) {
      layout.suffixText = fitLayoutText(input.suffix, layout.width);
      layout.suffixY = firstContentYAboveFooter - 1;
      layout.suffixX =
          std::max(0, layout.width - utf8DisplayWidth(layout.suffixText));
    }

    const int controlsBottom =
        (layout.suffixY >= 0 ? layout.suffixY : firstContentYAboveFooter) - 1;
    const int visibleControlLines =
        std::min(controlLineCount, std::max(0, controlsBottom + 1));
    const int controlsTop = controlsBottom - visibleControlLines + 1;
    for (const PendingControl& item : pending) {
      // Controls are ordered by workflow priority. On a short surface retain
      // complete leading rows instead of bottom-aligning later commands and
      // pushing the primary transport/edit controls above the viewport.
      if (item.line >= visibleControlLines) break;
      layout.controls.push_back(placeControl(item, controlsTop + item.line));
    }

    const int titleBaseY =
        visibleControlLines == 0
            ? ((layout.suffixY >= 0 ? layout.suffixY
                                    : firstContentYAboveFooter) -
               1)
            : (controlsTop - 1);
    const int titleSlots = std::max(0, titleBaseY + 1);
    const int titleLineCount =
        std::min(static_cast<int>(titleLines.size()), titleSlots);
    const int firstTitleLine =
        static_cast<int>(titleLines.size()) - titleLineCount;
    placeTitleLines(titleBaseY - titleLineCount + 1, firstTitleLine,
                    titleLineCount);
  } else {
    const int titleLineCount = static_cast<int>(titleLines.size());
    layout.height = titleLineCount + controlLineCount + (hasSuffix ? 1 : 0) +
                    reservedRowsAboveProgress + 1;
    placeTitleLines(0, 0, titleLineCount);

    const int controlsTop = titleLineCount;
    for (const PendingControl& item : pending) {
      layout.controls.push_back(placeControl(item, controlsTop + item.line));
    }

    if (hasSuffix) {
      layout.suffixText = fitLayoutText(input.suffix, layout.width);
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
    if (y >= 0) layout.topY = std::min(layout.topY, y);
  };
  for (const auto& line : layout.titleLines) useTop(line.y);
  useTop(layout.suffixY);
  for (const auto& item : layout.controls) useTop(item.y);
  return layout;
}

OverlayCellLayout layoutOverlayControlCells(
    const std::vector<OverlayCellControlInput>& controls, int width) {
  OverlayCellLayout layout;
  layout.width = std::max(1, width);

  int controlLineCount = 0;
  const std::vector<PendingControl> pending =
      wrapControls(controls, layout.width, &controlLineCount);
  layout.height = controlLineCount;
  layout.progressBarX = -1;
  layout.progressBarY = -1;
  layout.progressBarWidth = 0;
  layout.topY = pending.empty() ? -1 : 0;

  layout.controls.reserve(pending.size());
  for (const PendingControl& item : pending) {
    layout.controls.push_back(placeControl(item, item.line));
  }
  return layout;
}

OverlayCellLayout layoutOverlayDialogCells(
    const OverlayDialogLayoutInput& input) {
  OverlayCellLayout result;
  result.width = std::max(1, input.width);
  result.height = std::max(0, input.height);

  text_grid_dialog_layout::Content content;
  content.title = input.title;
  content.text.reserve(input.text.size());
  for (const std::string& line : input.text) {
    content.text.push_back(
        {line, text_grid_dialog_layout::TextTone::Normal});
  }

  std::size_t selectedIndex = 0;
  content.buttons.reserve(input.buttons.size());
  for (std::size_t index = 0; index < input.buttons.size(); ++index) {
    const OverlayDialogButtonInput& button = input.buttons[index];
    content.buttons.push_back(
        {static_cast<text_grid_dialog_layout::ButtonId>(index + 1),
         button.label, button.compactLabel});
    if (button.selected) selectedIndex = index;
  }

  const text_grid_dialog_layout::Layout dialog =
      text_grid_dialog_layout::layoutContent(
          content, selectedIndex, 0,
          text_grid_dialog_layout::Bounds{result.width, result.height, 0});
  if (!dialog.valid) return result;

  OverlayCellDialogLayout presentation;
  presentation.x = dialog.x;
  presentation.y = dialog.y;
  presentation.width = dialog.width;
  presentation.height = dialog.height;
  presentation.titleX = dialog.x + 2;
  presentation.titleY = dialog.titleY;
  presentation.title = fitLayoutText(content.title, dialog.innerWidth);

  const int endLine = std::min(
      static_cast<int>(dialog.contentLines.size()),
      dialog.firstContentLine + dialog.visibleContentRows);
  for (int line = dialog.firstContentLine; line < endLine; ++line) {
    presentation.contentLines.push_back(OverlayCellTextLine{
        dialog.x + 2, dialog.contentY + line - dialog.firstContentLine,
        fitLayoutText(dialog.contentLines[static_cast<std::size_t>(line)].text,
                      dialog.innerWidth)});
  }

  result.controls.reserve(dialog.buttons.size());
  for (const text_grid_dialog_layout::ButtonBounds& placement :
       dialog.buttons) {
    if (placement.index >= input.buttons.size() ||
        placement.index >= content.buttons.size()) {
      continue;
    }
    const OverlayDialogButtonInput& button = input.buttons[placement.index];
    const std::string& label =
        text_grid_button_layout::labelFor(content.buttons[placement.index],
                                          placement);
    result.controls.push_back(OverlayCellControlLayoutItem{
        button.id,
        fitControlText("[ " + label + " ]", placement.width),
        placement.x,
        placement.y >= 0 ? placement.y : dialog.buttonY,
        placement.width,
        button.selected,
        button.hovered,
        button.enabled});
  }

  if (result.controls.empty()) return result;
  result.topY = dialog.y;
  result.dialog = std::move(presentation);
  return result;
}

}  // namespace playback_overlay
