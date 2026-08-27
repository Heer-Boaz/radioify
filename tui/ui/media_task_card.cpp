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
                       const MediaTaskCardStyles& styles) {
  if (screenWidth < 4 || screenHeight - top < 3) return;

  const std::string cancelHint = model.cancellable ? "F8: Cancel" : "";
  int contentWidth = std::max(
      {utf8DisplayWidth(model.title), utf8DisplayWidth(model.sourceName),
       utf8DisplayWidth(model.detail), utf8DisplayWidth(cancelHint)});
  const int desiredWidth = std::max(46, contentWidth + 4);
  const int popupWidth = std::clamp(desiredWidth, 4, screenWidth);
  const int innerWidth = std::max(1, popupWidth - 2);

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
        std::max(0, innerWidth - utf8DisplayWidth(percentText) - 4);
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
  if (model.cancellable) {
    lines.push_back({cancelHint, styles.secondary});
  }

  const int availableHeight = screenHeight - top;
  const int popupHeight = std::min(
      static_cast<int>(lines.size()) + 2, availableHeight);
  const int x0 = std::max(0, screenWidth - popupWidth - 1);
  const int y0 = top;

  for (int y = 0; y < popupHeight; ++y) {
    screen.writeRun(x0, y0 + y, popupWidth, L' ', styles.background);
  }
  screen.writeChar(x0, y0, L'+', styles.secondary);
  screen.writeRun(x0 + 1, y0, popupWidth - 2, L'-', styles.secondary);
  screen.writeChar(x0 + popupWidth - 1, y0, L'+', styles.secondary);
  screen.writeChar(x0, y0 + popupHeight - 1, L'+', styles.secondary);
  screen.writeRun(x0 + 1, y0 + popupHeight - 1, popupWidth - 2, L'-',
                  styles.secondary);
  screen.writeChar(x0 + popupWidth - 1, y0 + popupHeight - 1, L'+',
                   styles.secondary);
  for (int y = 1; y < popupHeight - 1; ++y) {
    screen.writeChar(x0, y0 + y, L'|', styles.secondary);
    screen.writeChar(x0 + popupWidth - 1, y0 + y, L'|', styles.secondary);
  }

  const int visibleLines =
      std::min(static_cast<int>(lines.size()), popupHeight - 2);
  for (int line = 0; line < visibleLines; ++line) {
    screen.writeText(
        x0 + 1, y0 + 1 + line,
        fitLine(lines[static_cast<size_t>(line)].first, innerWidth),
        lines[static_cast<size_t>(line)].second);
  }
}
