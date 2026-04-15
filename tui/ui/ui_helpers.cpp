#include "ui_helpers.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "runtime_helpers.h"

std::optional<std::string> getEnvString(const char* name) {
  if (!name || name[0] == '\0') return std::nullopt;
#ifdef _WIN32
  size_t required = 0;
  if (getenv_s(&required, nullptr, 0, name) != 0 || required == 0) {
    return std::nullopt;
  }
  std::vector<char> buffer(required);
  if (getenv_s(&required, buffer.data(), buffer.size(), name) != 0 ||
      required == 0) {
    return std::nullopt;
  }
  if (buffer.empty() || buffer[0] == '\0') return std::nullopt;
  return std::string(buffer.data());
#else
  const char* value = std::getenv(name);
  if (!value || value[0] == '\0') return std::nullopt;
  return std::string(value);
#endif
}

std::FILE* openFileUtf8(const std::filesystem::path& path, const char* mode) {
  if (path.empty() || !mode || mode[0] == '\0') return nullptr;
#ifdef _WIN32
  std::wstring wpath = path.wstring();
  std::wstring wmode;
  for (const char* p = mode; *p != '\0'; ++p) {
    wmode.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
  }
  std::FILE* file = nullptr;
  if (_wfopen_s(&file, wpath.c_str(), wmode.c_str()) != 0) {
    return nullptr;
  }
  return file;
#else
  const std::string pathUtf8 = toUtf8String(path);
  return std::fopen(pathUtf8.c_str(), mode);
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

ProgressTextLayout buildProgressTextLayout(double displaySec,
                                           double totalSec,
                                           const std::string& status,
                                           int volPct,
                                           int width) {
  ProgressTextLayout out;
  std::string volStr = " Vol: " + std::to_string(volPct) + "%";
  out.suffix = formatTime(displaySec) + " / " + formatTime(totalSec) + " " +
               status + volStr;
  int suffixWidth = utf8CodepointCount(out.suffix);
  int barWidth = width - suffixWidth - 3;
  if (barWidth < 10) {
    out.suffix = formatTime(displaySec) + "/" + formatTime(totalSec) + " " +
                 status + " V:" + std::to_string(volPct) + "%";
    suffixWidth = utf8CodepointCount(out.suffix);
    barWidth = width - suffixWidth - 3;
  }
  if (barWidth < 10) {
    out.suffix = formatTime(displaySec) + " V:" + std::to_string(volPct) + "%";
    suffixWidth = utf8CodepointCount(out.suffix);
    barWidth = width - suffixWidth - 3;
  }
  if (barWidth < 5) {
    out.suffix.clear();
    barWidth = width - 2;
  }
  int maxBar = std::max(5, width - 2);
  out.barWidth = std::clamp(barWidth, 5, maxBar);
  return out;
}

AsciiArtLayout fitAsciiArtLayout(int srcWidth,
                                 int srcHeight,
                                 int maxWidthChars,
                                 int maxHeightChars) {
  AsciiArtLayout out;
  if (srcWidth <= 0 || srcHeight <= 0 || maxWidthChars <= 0 ||
      maxHeightChars <= 0) {
    return out;
  }

  const int maxSrcW = std::max(1, srcWidth / 2);
  const int maxSrcH = std::max(1, srcHeight / 4);
  const int clampedMaxW = std::max(1, std::min(maxWidthChars, maxSrcW));
  const int clampedMaxH = std::max(1, std::min(maxHeightChars, maxSrcH));

  const double scaleW =
      (2.0 * static_cast<double>(clampedMaxW)) / static_cast<double>(srcWidth);
  const double scaleH =
      (4.0 * static_cast<double>(clampedMaxH)) / static_cast<double>(srcHeight);
  const double scale = std::min({scaleW, scaleH, 1.0});

  int fittedW = static_cast<int>(
      std::lround(static_cast<double>(srcWidth) * scale / 2.0));
  int fittedH = static_cast<int>(
      std::lround(static_cast<double>(srcHeight) * scale / 4.0));
  fittedW = std::clamp(fittedW, 1, clampedMaxW);
  fittedH = std::clamp(fittedH, 1, clampedMaxH);

  out.width = fittedW;
  out.height = fittedH;
  return out;
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
