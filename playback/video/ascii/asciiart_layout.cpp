#include "asciiart_layout.h"

#include <algorithm>
#include <cmath>

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
