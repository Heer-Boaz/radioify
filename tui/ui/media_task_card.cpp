#include "tui/ui/media_task_card.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

#include "tui/ui/ui_helpers.h"

void drawMediaTaskCard(ConsoleScreen& screen, int screenWidth,
                       int screenHeight, int top,
                       const MediaTaskCardModel& model,
                       const tui_media_task_panel::State& state,
                       const MediaTaskCardStyles& styles) {
  const tui_media_task_panel::Layout layout =
      tui_media_task_panel::layout({screenWidth, screenHeight, top}, model);
  if (!layout.valid) return;

  std::vector<std::pair<std::string, Style>> lines;
  lines.push_back({model.title, styles.title});
  lines.push_back({model.sourceName, styles.secondary});
  if (!model.detail.empty()) {
    lines.push_back({model.detail, styles.secondary});
  }

  std::string progressLine;
  if (model.progress) {
    const float progress = std::clamp(*model.progress, 0.0f, 1.0f);
    const int percent =
        static_cast<int>(std::round(progress * 100.0f));
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
  lines.push_back({std::move(progressLine), styles.progress});
  for (int y = 0; y < layout.height; ++y) {
    screen.writeRun(layout.x, layout.y + y, layout.width, L' ',
                    styles.background);
  }
  screen.writeChar(layout.x, layout.y, L'+', styles.secondary);
  screen.writeRun(layout.x + 1, layout.y, layout.width - 2, L'-',
                  styles.secondary);
  screen.writeChar(layout.x + layout.width - 1, layout.y, L'+',
                   styles.secondary);
  screen.writeChar(layout.x, layout.y + layout.height - 1, L'+',
                   styles.secondary);
  screen.writeRun(layout.x + 1, layout.y + layout.height - 1,
                  layout.width - 2, L'-',
                  styles.secondary);
  screen.writeChar(layout.x + layout.width - 1,
                   layout.y + layout.height - 1, L'+',
                   styles.secondary);
  for (int y = 1; y < layout.height - 1; ++y) {
    screen.writeChar(layout.x, layout.y + y, L'|', styles.secondary);
    screen.writeChar(layout.x + layout.width - 1, layout.y + y, L'|',
                     styles.secondary);
  }

  const int visibleLines =
      std::min(static_cast<int>(lines.size()), layout.height - 2);
  for (int line = 0; line < visibleLines; ++line) {
    screen.writeText(
        layout.x + 1, layout.y + 1 + line,
        fitLine(lines[static_cast<size_t>(line)].first, layout.innerWidth),
        lines[static_cast<size_t>(line)].second);
  }

  const std::vector<tui_button_row::Button> actions =
      tui_media_task_panel::actionsFor(model);
  for (const tui_button_row::Placement& buttonBounds :
       layout.buttons.buttons) {
    if (buttonBounds.index >= actions.size()) continue;
    const bool selected = state.hoveredButton() == buttonBounds.index;
    const Style style = selected ? styles.selectedButton : styles.button;
    const std::string label = "[ " + actions[buttonBounds.index].label + " ]";
    screen.writeRun(buttonBounds.x, layout.buttons.y, buttonBounds.width, L' ',
                    style);
    screen.writeText(buttonBounds.x, layout.buttons.y,
                     fitLine(label, buttonBounds.width), style);
  }
}
