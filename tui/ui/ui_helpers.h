#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "asciiart_layout.h"
#include "consolescreen.h"
#include "unicode_display_width.h"

struct BufferCell {
  wchar_t ch = L' ';
  Style style{};
};

struct ProgressTextLayout {
  std::string suffix;
  int barWidth = 5;
};

struct ProgressFooterStyles {
  Style normal;
  Style progressEmpty;
  Style progressFrame;
  Style alert;
  Style accent;
  Color progressStart;
  Color progressEnd;
};

struct ProgressFooterInput {
  double displaySec = 0.0;
  double totalSec = -1.0;
  double ratio = 0.0;
  int volPct = 0;
  int width = 0;
  int progressY = -1;
  int peakY = -1;
  float unclippedOutputPeak = 0.0f;
};

struct ProgressFooterRenderResult {
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
};

struct ProgressBarHitTestInput {
  double x = 0.0;
  double y = 0.0;
  int barX = -1;
  int barY = -1;
  int barWidth = 0;
  double unitWidth = 1.0;
  double unitHeight = 1.0;
};

struct BracketButtonLabels {
  std::string normal;
  std::string hover;
  int width = 0;
};

float clamp01(float v);
Color scaleColor(const Color& color, float amount);
Color lerpColor(const Color& a, const Color& b, float t);
BracketButtonLabels makeBracketButtonLabels(const std::string& text);

int utf8CodepointCount(const std::string& s);
std::string utf8Take(const std::string& s, int count);
std::string utf8Slice(const std::string& s, int start, int count);
std::string fitLine(const std::string& s, int width);
std::vector<std::string> wrapLine(const std::string& s, int width);
int wrappedLineCount(const std::string& s, int width);
std::string formatTime(double seconds);
ProgressTextLayout buildProgressTextLayout(double displaySec,
                                           double totalSec,
                                           int volPct,
                                           int width);
std::vector<BufferCell> renderProgressBarCells(double ratio,
                                               int width,
                                               const Style& emptyStyle,
                                               const Color& startColor,
                                               const Color& endColor);
ProgressFooterRenderResult renderProgressFooter(
    ConsoleScreen& screen, const ProgressFooterInput& input,
    const ProgressFooterStyles& styles);
inline std::optional<double> progressBarRatioAt(
    const ProgressBarHitTestInput& input, bool clampToBar = false) {
  if (input.barWidth <= 0 || input.barX < 0 || input.barY < 0) {
    return std::nullopt;
  }
  const double unitWidth = std::max(1.0, input.unitWidth);
  const double unitHeight = std::max(1.0, input.unitHeight);
  const double left = static_cast<double>(input.barX) * unitWidth;
  const double top = static_cast<double>(input.barY) * unitHeight;
  const double width = static_cast<double>(input.barWidth) * unitWidth;
  double x = input.x;
  if (clampToBar) {
    x = std::clamp(x, left, left + std::max(0.0, width - 1.0));
  } else if (x < left || x >= left + width || input.y < top ||
             input.y >= top + unitHeight) {
    return std::nullopt;
  }
  const double denominator = std::max(1.0, width - 1.0);
  return std::clamp((x - left) / denominator, 0.0, 1.0);
}
