#ifndef ASCIIART_H
#define ASCIIART_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "consolescreen.h"
#include "videocolor.h"

struct AsciiArt {
  int width = 0;
  int height = 0;
  struct AsciiCell {
    wchar_t ch = L' ';
    Color fg{255, 255, 255};
    Color bg{0, 0, 0};
    bool hasBg = false;
  };
  std::vector<AsciiCell> cells;
};

std::string defaultAsciiRamp();
char rampCharFromLuma(float luma, const std::string& ramp);

bool renderAsciiArt(const std::filesystem::path& path,
                    int maxWidth,
                    int maxHeight,
                    AsciiArt& out,
                    std::string* error);

bool renderAsciiArtFromEncodedImageBytes(const uint8_t* bytes,
                                         size_t size,
                                         int maxWidth,
                                         int maxHeight,
                                         AsciiArt& out,
                                         std::string* error);

bool renderAsciiArtFromRgba(const uint8_t* rgba,
                            int width,
                            int height,
                            int maxWidth,
                            int maxHeight,
                            AsciiArt& out,
                            bool assumeOpaque = false);

enum class YuvFormat {
  NV12,
  P010,
};

bool renderAsciiArtFromYuv(const uint8_t* data,
                           int width,
                           int height,
                           int stride,
                           int planeHeight,
                           YuvFormat format,
                           bool fullRange,
                           YuvMatrix yuvMatrix,
                           YuvTransfer yuvTransfer,
                           int maxWidth,
                           int maxHeight,
                           AsciiArt& out);

#endif
