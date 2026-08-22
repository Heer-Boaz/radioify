#pragma once

#include <cstdint>

namespace subtitle_pixel_compositor {

struct Bgra8 {
  std::uint8_t b = 0;
  std::uint8_t g = 0;
  std::uint8_t r = 0;
  std::uint8_t a = 0;
};

struct Composition {
  Bgra8 pixel;
  std::uint8_t textCoverage = 0;
};

// GDI rasterizes text against an opaque destination and does not expose an
// alpha channel. Rendering the same glyphs over black and white yields the
// effective premultiplied color and coverage. This function reconstructs a
// straight-alpha pixel, applies the configured text opacity, and composites
// it over the subtitle background.
Composition composeOpaqueTextPair(Bgra8 renderedOnBlack,
                                  Bgra8 renderedOnWhite,
                                  std::uint8_t textOpacity,
                                  Bgra8 background);

}  // namespace subtitle_pixel_compositor
