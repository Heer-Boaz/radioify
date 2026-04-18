#include "asciiart_layout.h"

#include <algorithm>
#include <cmath>

AsciiArtLayout fitAsciiArtLayout(int srcWidth,
                                 int srcHeight,
                                 int maxWidthChars,
                                 int maxHeightChars,
                                 int cellPixelWidth,
                                 int cellPixelHeight) {
  AsciiArtLayout out;
  if (srcWidth <= 0 || srcHeight <= 0 || maxWidthChars <= 0 ||
      maxHeightChars <= 0) {
    return out;
  }

  const int cellW = std::max(1, cellPixelWidth);
  const int cellH = std::max(1, cellPixelHeight);
  const int clampedMaxW = std::max(1, maxWidthChars);
  const int clampedMaxH = std::max(1, maxHeightChars);

  const double scaleW =
      (static_cast<double>(cellW) * static_cast<double>(clampedMaxW)) /
      static_cast<double>(srcWidth);
  const double scaleH =
      (static_cast<double>(cellH) * static_cast<double>(clampedMaxH)) /
      static_cast<double>(srcHeight);
  const double scale = std::min(scaleW, scaleH);

  int fittedW = static_cast<int>(
      std::lround(static_cast<double>(srcWidth) * scale /
                  static_cast<double>(cellW)));
  int fittedH = static_cast<int>(
      std::lround(static_cast<double>(srcHeight) * scale /
                  static_cast<double>(cellH)));
  fittedW = std::clamp(fittedW, 1, clampedMaxW);
  fittedH = std::clamp(fittedH, 1, clampedMaxH);

  out.width = fittedW;
  out.height = fittedH;
  return out;
}
