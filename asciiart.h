#ifndef ASCIIART_H
#define ASCIIART_H

#include <filesystem>
#include <string>
#include <vector>

#include "consolescreen.h"

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

bool renderAsciiArtFromRgba(const uint8_t* rgba,
                            int width,
                            int height,
                            int maxWidth,
                            int maxHeight,
                            AsciiArt& out,
                            bool assumeOpaque = false);

#endif
