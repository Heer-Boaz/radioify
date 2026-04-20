#include "asciiart_layout.h"

#include <algorithm>
#include <cmath>

AsciiArtLayout fitAsciiArtLayout(int srcWidth,
                                 int srcHeight,
                                 int maxWidthChars,
                                 int maxHeightChars,
                                 double cellPixelWidth,
                                 double cellPixelHeight) {
  AsciiArtLayout out;
  const double scaleW =
      (cellPixelWidth * static_cast<double>(maxWidthChars)) /
      static_cast<double>(srcWidth);
  const double scaleH =
      (cellPixelHeight * static_cast<double>(maxHeightChars)) /
      static_cast<double>(srcHeight);
  const double scale = std::min(scaleW, scaleH);

  out.width = static_cast<int>(
      std::lround(static_cast<double>(srcWidth) * scale /
                  cellPixelWidth));
  out.height = static_cast<int>(
      std::lround(static_cast<double>(srcHeight) * scale /
                  cellPixelHeight));
  return out;
}
