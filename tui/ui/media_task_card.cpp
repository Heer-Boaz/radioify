#include "tui/ui/media_task_card.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "tui/ui/ui_helpers.h"

void drawMediaTaskCard(ConsoleScreen& screen, int screenWidth, int screenHeight,
                       int top, const MediaTaskCardModel& model,
                       const tui_media_task_panel::State& state,
                       const MediaTaskCardStyles& styles) {
  const tui_media_task_panel::Layout layout =
      tui_media_task_panel::layout({screenWidth, screenHeight, top}, model);
  if (!layout.valid) return;

  std::string progressLine;
  if (model.progress) {
    const float progress = std::clamp(*model.progress, 0.0f, 1.0f);
    const int percent = static_cast<int>(std::round(progress * 100.0f));
    const std::string percentText = std::to_string(percent) + "%";
    const int barCells =
        std::max(0, layout.innerWidth - utf8DisplayWidth(percentText) - 4);
    if (barCells >= 4) {
      const int filled = std::clamp(
          static_cast<int>(std::round(barCells * progress)), 0, barCells);
      progressLine = "[";
      progressLine.append(static_cast<size_t>(filled), '#');
      progressLine.append(static_cast<size_t>(barCells - filled), '.');
      progressLine += "] " + percentText;
    } else {
      progressLine = percentText;
    }
  } else {
    progressLine = "Working...";
  }
  std::vector<std::pair<std::string, Style>> lines;
  lines.push_back({model.title, styles.title});
  if (layout.contentRows >= 2) {
    lines.push_back({model.sourceName, styles.secondary});
  }
  const int engineRowsRequired = model.detail.empty() ? 4 : 5;
  if (layout.contentRows >= engineRowsRequired && !model.engineName.empty()) {
    lines.push_back({"Engine: " + model.engineName, styles.secondary});
  }
  if (layout.contentRows >= 4 && !model.detail.empty()) {
    lines.push_back({model.detail, styles.secondary});
  }
  if (layout.contentRows >= 3) {
    lines.push_back({std::move(progressLine), styles.progress});
  }
  for (int y = 0; y < layout.height; ++y) {
    screen.writeRun(layout.x, layout.y + y, layout.width, L' ',
                    styles.background);
  }
  const Style border =
      state.focused() ? styles.focusedBorder : styles.secondary;
  screen.writeChar(layout.x, layout.y, L'+', border);
  screen.writeRun(layout.x + 1, layout.y, layout.width - 2, L'-', border);
  screen.writeChar(layout.x + layout.width - 1, layout.y, L'+', border);
  screen.writeChar(layout.x, layout.y + layout.height - 1, L'+', border);
  screen.writeRun(layout.x + 1, layout.y + layout.height - 1, layout.width - 2,
                  L'-', border);
  screen.writeChar(layout.x + layout.width - 1, layout.y + layout.height - 1,
                   L'+', border);
  for (int y = 1; y < layout.height - 1; ++y) {
    screen.writeChar(layout.x, layout.y + y, L'|', border);
    screen.writeChar(layout.x + layout.width - 1, layout.y + y, L'|', border);
  }

  const int visibleLines =
      std::min(static_cast<int>(lines.size()), layout.contentRows);
  for (int line = 0; line < visibleLines; ++line) {
    screen.writeText(
        layout.x + 1, layout.y + 1 + line,
        fitLine(lines[static_cast<size_t>(line)].first, layout.innerWidth),
        lines[static_cast<size_t>(line)].second);
  }

  const std::vector<tui_button_row::Button> actions =
      tui_media_task_panel::actionsFor(model);
  for (const tui_button_row::Placement& buttonBounds : layout.buttons.buttons) {
    if (buttonBounds.index >= actions.size()) continue;
    const bool selected = state.highlightedButton() == buttonBounds.index;
    const Style style = selected ? styles.selectedButton : styles.button;
    const std::string label =
        "[ " + tui_button_row::labelFor(actions[buttonBounds.index],
                                          buttonBounds) +
        " ]";
    const int buttonY =
        buttonBounds.y >= 0 ? buttonBounds.y : layout.buttons.y;
    screen.writeRun(buttonBounds.x, buttonY, buttonBounds.width, L' ',
                    style);
    screen.writeText(buttonBounds.x, buttonY,
                     fitLine(label, buttonBounds.width), style);
  }
}

tui_media_task_panel::IndicatorLayout drawMediaTaskIndicator(
    ConsoleScreen& screen, int screenWidth, int y,
    const MediaTaskCardModel& model, const tui_media_task_panel::State& state,
    const MediaTaskCardStyles& styles) {
  const tui_media_task_panel::IndicatorLayout layout =
      tui_media_task_panel::indicatorLayout(screenWidth, y, model);
  if (!layout.valid) return layout;

  if (layout.statusWidth > 0) {
    screen.writeText(layout.x, layout.y,
                     fitLine(layout.statusText, layout.statusWidth),
                     styles.progress);
  }
  if (layout.actionVisible) {
    const Style actionStyle = state.indicatorHighlighted()
                                  ? styles.selectedButton
                                  : styles.button;
    screen.writeText(layout.showX, layout.y, "[ Show ]", actionStyle);
  }
  return layout;
}
