#include "video_transition_view.h"

#include <algorithm>
#include <cmath>
#include <string>

#include "consolescreen.h"
#include "runtime_helpers.h"
#include "ui_helpers.h"

namespace tui_video_transition_view {
namespace {

std::string stageMessage(playback_session::TransitionStage stage) {
  switch (stage) {
    case playback_session::TransitionStage::Opening:
      return "Preparing video playback...";
    case playback_session::TransitionStage::Cancelling:
      return "Cancelling video preparation...";
    case playback_session::TransitionStage::Closing:
      return "Closing video playback...";
  }
  return "Preparing video playback...";
}

std::string stageTitle(playback_session::TransitionStage stage) {
  switch (stage) {
    case playback_session::TransitionStage::Opening:
      return "Opening video";
    case playback_session::TransitionStage::Cancelling:
      return "Cancelling video";
    case playback_session::TransitionStage::Closing:
      return "Closing video";
  }
  return "Video";
}

void drawCenteredText(ConsoleScreen& screen, int width, int y,
                      const std::string& text, const Style& style) {
  const std::string fitted = fitLine(text, width);
  const int textWidth = utf8DisplayWidth(fitted);
  screen.writeText(std::max(0, (width - textWidth) / 2), y, fitted, style);
}

void drawIndeterminateBar(ConsoleScreen& screen, int x, int y, int width,
                          double activity, const Styles& styles) {
  const int segmentWidth = std::max(3, width / 5);
  const int travel = std::max(0, width - segmentWidth);
  const int segmentStart = static_cast<int>(std::lround(
      std::clamp(activity, 0.0, 1.0) * static_cast<double>(travel)));
  for (int index = 0; index < width; ++index) {
    if (index < segmentStart || index >= segmentStart + segmentWidth) {
      screen.writeChar(x + index, y, L' ', styles.progressEmpty);
      continue;
    }
    const float colorPosition =
        width > 1 ? static_cast<float>(index) / (width - 1) : 0.0f;
    screen.writeChar(
        x + index, y, L'\x2588',
        {lerpColor(styles.progressStart, styles.progressEnd, colorPosition),
         styles.progressEmpty.bg});
  }
}

}  // namespace

Layout draw(ConsoleScreen& screen,
            const playback_session::TransitionSnapshot& snapshot,
            const Styles& styles) {
  screen.updateSize();
  const int width = std::max(1, screen.width());
  const int height = std::max(1, screen.height());
  Layout result = layout(width, height, snapshot.stage);
  screen.clear(styles.base);

  drawCenteredText(screen, width, 0, stageTitle(snapshot.stage), styles.accent);
  if (height > 2) {
    drawCenteredText(screen, width, 2, toUtf8String(snapshot.file.filename()),
                     styles.dim);
  }

  const int messageY = std::clamp(height / 2 - 1, 1, std::max(1, height - 1));
  drawCenteredText(screen, width, messageY, stageMessage(snapshot.stage),
                   styles.dim);

  const int barWidth = std::min(32, width - 6);
  const int barY = messageY + 1;
  if (barWidth >= 5 && barY < height) {
    const int barX = std::max(0, (width - (barWidth + 2)) / 2);
    screen.writeChar(barX, barY, L'|', styles.progressFrame);
    drawIndeterminateBar(screen, barX + 1, barY, barWidth, snapshot.activity,
                         styles);
    screen.writeChar(barX + barWidth + 1, barY, L'|', styles.progressFrame);
  }

  if (result.cancel.width > 0) {
    constexpr char kCancelLabel[] = "[ Cancel ]";
    static_assert(sizeof(kCancelLabel) - 1 == 10);
    screen.writeText(result.cancel.x, result.cancel.y, kCancelLabel,
                     styles.accent);
  }
  return result;
}

}  // namespace tui_video_transition_view
