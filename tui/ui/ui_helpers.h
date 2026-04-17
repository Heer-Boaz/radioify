#pragma once

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "runtime_helpers.h"
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
  float peak = 0.0f;
  bool clipAlert = false;
};

struct ProgressFooterRenderResult {
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
};

struct ProgressBarHitTestInput {
  int x = 0;
  int y = 0;
  int barX = -1;
  int barY = -1;
  int barWidth = 0;
  int unitWidth = 1;
  int unitHeight = 1;
};

struct BracketButtonLabels {
  std::string normal;
  std::string hover;
  int width = 0;
};

std::optional<std::string> getEnvString(const char* name);
std::FILE* openFileUtf8(const std::filesystem::path& path, const char* mode);

float clamp01(float v);
Color scaleColor(const Color& color, float amount);
Color lerpColor(const Color& a, const Color& b, float t);
BracketButtonLabels makeBracketButtonLabels(const std::string& text);

int utf8CodepointCount(const std::string& s);
std::string utf8Take(const std::string& s, int count);
std::string utf8Slice(const std::string& s, int start, int count);
std::string fitLine(const std::string& s, int width);
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
bool progressBarRatioAt(const ProgressBarHitTestInput& input,
                        double* outRatio);
