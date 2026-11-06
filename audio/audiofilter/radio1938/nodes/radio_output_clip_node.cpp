#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>

void RadioOutputClipNode::init(Radio1938& radio, RadioInitContext&) {
  auto& output = radio.output;
  output.clipOversampler.configure(radio.globals.oversampleFactor,
                                  radio.globals.oversampleCutoffFraction);
}

void RadioOutputClipNode::reset(Radio1938& radio) {
  auto& output = radio.output;
  output.clipOversampler.reset();
}

float RadioOutputClipNode::run(Radio1938& radio,
                               float y,
                               RadioSampleContext&) {
  auto& output = radio.output;
  const float out = output.clipOversampler.process(y, [&](float v) {
    const float t = radio.globals.outputClipThreshold;
    if (std::fabs(v) > t) radio.diagnostics.markOutputClip();
    return std::clamp(softClip(v, t), -t, t);
  });
  // Reconstruction can overshoot a clipped waveform. The output contract is
  // a hard digital sample ceiling after decimation, including its overshoot.
  const float threshold = radio.globals.outputClipThreshold;
  if (std::fabs(out) > threshold)
    radio.diagnostics.markOutputClip();
  return std::clamp(out, -threshold, threshold);
}
