#pragma once

#include <cstdint>

struct Color {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
};

struct Style {
  Color fg;
  Color bg;
};
