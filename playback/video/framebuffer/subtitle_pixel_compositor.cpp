#include "subtitle_pixel_compositor.h"

#include <algorithm>
#include <cmath>

namespace subtitle_pixel_compositor {
namespace {

double channelCoverage(std::uint8_t black, std::uint8_t white) {
  return std::clamp((static_cast<double>(black) + 255.0 - white) / 255.0,
                    0.0, 1.0);
}

std::uint8_t toByte(double value) {
  return static_cast<std::uint8_t>(
      std::lround(std::clamp(value, 0.0, 1.0) * 255.0));
}

}  // namespace

Composition composeOpaqueTextPair(Bgra8 renderedOnBlack,
                                  Bgra8 renderedOnWhite,
                                  std::uint8_t textOpacity,
                                  Bgra8 background) {
  const double coverage =
      (channelCoverage(renderedOnBlack.b, renderedOnWhite.b) +
       channelCoverage(renderedOnBlack.g, renderedOnWhite.g) +
       channelCoverage(renderedOnBlack.r, renderedOnWhite.r)) /
      3.0;
  const double opacity = static_cast<double>(textOpacity) / 255.0;
  const double textAlpha = coverage * opacity;
  const double backgroundAlpha =
      static_cast<double>(background.a) / 255.0;
  const double outputAlpha =
      textAlpha + backgroundAlpha * (1.0 - textAlpha);

  const auto compositeChannel = [&](std::uint8_t textPremultiplied,
                                    std::uint8_t backgroundChannel)
      -> std::uint8_t {
    const double text =
        (std::min)(static_cast<double>(textPremultiplied) / 255.0,
                   coverage) *
        opacity;
    const double backdrop =
        (static_cast<double>(backgroundChannel) / 255.0) * backgroundAlpha;
    const double premultiplied = text + backdrop * (1.0 - textAlpha);
    return outputAlpha > 0.0 ? toByte(premultiplied / outputAlpha)
                             : std::uint8_t{0};
  };

  Composition result;
  result.textCoverage = toByte(coverage);
  result.pixel = {
      compositeChannel(renderedOnBlack.b, background.b),
      compositeChannel(renderedOnBlack.g, background.g),
      compositeChannel(renderedOnBlack.r, background.r),
      toByte(outputAlpha)};
  return result;
}

}  // namespace subtitle_pixel_compositor
