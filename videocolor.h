#ifndef VIDEO_COLOR_H
#define VIDEO_COLOR_H

#include <cstdint>

enum class YuvMatrix : uint32_t {
  Bt709 = 0,
  Bt601 = 1,
  Bt2020 = 2,
};

enum class YuvTransfer : uint32_t {
  Sdr = 0,
  Pq = 1,
  Hlg = 2,
};

#endif
