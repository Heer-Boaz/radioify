#pragma once

#include <cstdint>
#include <vector>

#include "consolescreen.h"

bool renderScreenGridToBitmap(const ScreenCell* cells, int cols, int rows,
                              int width, int height,
                              std::vector<uint8_t>* outPixels);
