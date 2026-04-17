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
                                           const std::string& status,
                                           int volPct,
                                           int width);
std::vector<BufferCell> renderProgressBarCells(double ratio,
                                               int width,
                                               const Style& emptyStyle,
                                               const Color& startColor,
                                               const Color& endColor);
