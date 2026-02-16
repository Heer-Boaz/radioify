#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "consolescreen.h"

struct BufferCell {
  wchar_t ch = L' ';
  Style style{};
};

struct ProgressTextLayout {
  std::string suffix;
  int barWidth = 5;
};

struct AsciiArtLayout {
  int width = 1;
  int height = 1;
};

std::string toUtf8String(const std::filesystem::path& p);

float clamp01(float v);
Color lerpColor(const Color& a, const Color& b, float t);

int utf8CodepointCount(const std::string& s);
std::string utf8Take(const std::string& s, int count);
std::string utf8Slice(const std::string& s, int start, int count);
std::string fitLine(const std::string& s, int width);
std::string formatTime(double seconds);
ProgressTextLayout buildProgressTextLayout(double displaySec,
                                           double totalSec,
                                           const std::string& status,
                                           int volPct,
                                           float radioGain,
                                           int width);
AsciiArtLayout fitAsciiArtLayout(int srcWidth,
                                 int srcHeight,
                                 int maxWidthChars,
                                 int maxHeightChars);

std::vector<BufferCell> renderProgressBarCells(double ratio,
                                               int width,
                                               const Style& emptyStyle,
                                               const Color& startColor,
                                               const Color& endColor);
