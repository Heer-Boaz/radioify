#ifndef ASCIIART_LAYOUT_H
#define ASCIIART_LAYOUT_H

struct AsciiArtLayout {
  int width = 1;
  int height = 1;
};

AsciiArtLayout fitAsciiArtLayout(int srcWidth,
                                 int srcHeight,
                                 int maxWidthChars,
                                 int maxHeightChars,
                                 double cellPixelWidth,
                                 double cellPixelHeight);

#endif
