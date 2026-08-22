#include "playback/video/framebuffer/subtitle_pixel_compositor.h"

#include <cstdlib>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "subtitle_pixel_compositor_tests: " << message << '\n';
    return false;
  }
  return true;
}

bool nearByte(std::uint8_t value, int expected, int tolerance = 1) {
  return std::abs(static_cast<int>(value) - expected) <= tolerance;
}

}  // namespace

int main() {
  using subtitle_pixel_compositor::Bgra8;
  using subtitle_pixel_compositor::composeOpaqueTextPair;

  bool ok = true;
  const auto transparent = composeOpaqueTextPair(
      Bgra8{0, 0, 0, 0}, Bgra8{255, 255, 255, 0}, 255,
      Bgra8{0, 0, 0, 0});
  ok &= expect(transparent.textCoverage == 0 && transparent.pixel.a == 0,
               "unchanged black/white renders must reconstruct transparency");

  const auto red = composeOpaqueTextPair(
      Bgra8{0, 0, 255, 0}, Bgra8{0, 0, 255, 0}, 255,
      Bgra8{0, 0, 0, 0});
  ok &= expect(red.textCoverage == 255 && red.pixel.r == 255 &&
                   red.pixel.g == 0 && red.pixel.b == 0 &&
                   red.pixel.a == 255,
               "opaque colored text must retain straight color and alpha");

  const auto antialiasedWhite = composeOpaqueTextPair(
      Bgra8{128, 128, 128, 0}, Bgra8{255, 255, 255, 0}, 255,
      Bgra8{0, 0, 0, 0});
  ok &= expect(nearByte(antialiasedWhite.textCoverage, 128) &&
                   antialiasedWhite.pixel.r == 255 &&
                   nearByte(antialiasedWhite.pixel.a, 128),
               "antialiased coverage must be reconstructed continuously");

  const auto halfOpacity = composeOpaqueTextPair(
      Bgra8{255, 255, 255, 0}, Bgra8{255, 255, 255, 0}, 128,
      Bgra8{0, 0, 0, 0});
  ok &= expect(halfOpacity.pixel.r == 255 &&
                   nearByte(halfOpacity.pixel.a, 128),
               "configured text opacity must scale reconstructed alpha");

  const auto overBackground = composeOpaqueTextPair(
      Bgra8{255, 255, 255, 0}, Bgra8{255, 255, 255, 0}, 128,
      Bgra8{255, 0, 0, 128});
  ok &= expect(nearByte(overBackground.pixel.a, 192) &&
                   nearByte(overBackground.pixel.r, 170, 2) &&
                   nearByte(overBackground.pixel.g, 170, 2) &&
                   overBackground.pixel.b == 255,
               "text must source-over the configured translucent background");

  return ok ? 0 : 1;
}
