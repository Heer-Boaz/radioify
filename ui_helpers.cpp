#include "ui_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

std::string toUtf8String(const std::filesystem::path& p) {
#ifdef _WIN32
  auto u8 = p.u8string();
  return std::string(u8.begin(), u8.end());
#else
  return p.string();
#endif
}

std::string formatTime(double seconds) {
  if (!std::isfinite(seconds) || seconds < 0) return "--:--";
  int sec = static_cast<int>(std::floor(seconds + 1e-6));
  int hours = sec / 3600;
  sec %= 3600;
  int min = sec / 60;
  sec %= 60;
  char buf[32];
  if (hours > 0) {
    std::snprintf(buf, sizeof(buf), "%d:%02d:%02d", hours, min, sec);
  } else {
    std::snprintf(buf, sizeof(buf), "%d:%02d", min, sec);
  }
  return buf;
}

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

Color lerpColor(const Color& a, const Color& b, float t) {
  float tt = clamp01(t);
  Color out;
  out.r = static_cast<uint8_t>(std::lround(a.r + (b.r - a.r) * tt));
  out.g = static_cast<uint8_t>(std::lround(a.g + (b.g - a.g) * tt));
  out.b = static_cast<uint8_t>(std::lround(a.b + (b.b - a.b) * tt));
  return out;
}

static size_t utf8Next(const std::string& s, size_t i) {
  if (i >= s.size()) return s.size();
  unsigned char c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) return i + 1;
  if ((c & 0xE0) == 0xC0) return std::min(i + 2, s.size());
  if ((c & 0xF0) == 0xE0) return std::min(i + 3, s.size());
  if ((c & 0xF8) == 0xF0) return std::min(i + 4, s.size());
  return i + 1;
}

int utf8CodepointCount(const std::string& s) {
  int count = 0;
  size_t i = 0;
  while (i < s.size()) {
    i = utf8Next(s, i);
    ++count;
  }
  return count;
}

std::string utf8Take(const std::string& s, int count) {
  if (count <= 0) return "";
  size_t i = 0;
  int c = 0;
  for (; i < s.size() && c < count; ++c) {
    i = utf8Next(s, i);
  }
  return s.substr(0, i);
}

std::string utf8Slice(const std::string& s, int start, int count) {
  if (count <= 0 || start < 0) return "";
  size_t i = 0;
  int c = 0;
  for (; i < s.size() && c < start; ++c) {
    i = utf8Next(s, i);
  }
  size_t startByte = i;
  int end = start + count;
  for (; i < s.size() && c < end; ++c) {
    i = utf8Next(s, i);
  }
  if (startByte >= s.size()) return "";
  return s.substr(startByte, i - startByte);
}

std::string fitLine(const std::string& s, int width) {
  if (width <= 0) return "";
  int count = utf8CodepointCount(s);
  if (count <= width) return s;
  if (width <= 1) return utf8Take(s, width);
  return utf8Take(s, width - 1) + "~";
}

static int quantizeCoverage(double value) {
  int quantized = static_cast<int>(std::round(value * 8.0 + 1e-7));
  return std::clamp(quantized, 0, 8);
}

static wchar_t glyphForCoverage(double coverage) {
  static const wchar_t kLeftBlocks[] = {L'\x258F', L'\x258E', L'\x258D',
                                        L'\x258C', L'\x258B', L'\x258A',
                                        L'\x2589', L'\x2588'};
  int idx = quantizeCoverage(coverage);
  if (idx <= 0) return L' ';
  if (idx >= 8) return L'\x2588';
  return kLeftBlocks[idx - 1];
}

std::vector<BufferCell> renderProgressBarCells(double ratio,
                                               int width,
                                               const Style& emptyStyle,
                                               const Color& startColor,
                                               const Color& endColor) {
  std::vector<BufferCell> cells;
  if (width <= 0) return cells;
  cells.resize(static_cast<size_t>(width));
  double clamped = std::clamp(ratio, 0.0, 1.0);
  double fill = clamped * width;
  for (int i = 0; i < width; ++i) {
    double coverage = std::clamp(fill - i, 0.0, 1.0);
    BufferCell cell;
    if (coverage <= 0.0) {
      cell.ch = L' ';
      cell.style = emptyStyle;
    } else {
      float t = width > 1 ? static_cast<float>(i) / (width - 1) : 0.0f;
      Color fg = lerpColor(startColor, endColor, t);
      cell.ch = glyphForCoverage(coverage);
      cell.style = {fg, emptyStyle.bg};
    }
    cells[static_cast<size_t>(i)] = cell;
  }
  return cells;
}
