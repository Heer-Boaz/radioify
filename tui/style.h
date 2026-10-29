#pragma once

#include <cstdint>

struct Color {
  std::uint8_t r = 0;
  std::uint8_t g = 0;
  std::uint8_t b = 0;
  // Optional BT.2020 RGB10 colour; top two bits are YuvTransfer (PQ/HLG).
  // r/g/b remain the SDR representation for terminal output.
  std::uint32_t hdr = 0;
};

struct Style {
  Color fg;
  Color bg;
};
