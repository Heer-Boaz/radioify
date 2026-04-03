#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>

namespace {

float scalePhysicalSpeakerVoltsToDigital(const Radio1938& radio,
                                         float speakerVolts) {
  if (!radio.graph.isEnabled(PassId::Power)) return speakerVolts;
  constexpr float kDigitalProgramPeakHeadroom = 1.12f;
  return speakerVolts /
         (requirePositiveFinite(radio.output.digitalReferenceSpeakerVoltsPeak) *
          kDigitalProgramPeakHeadroom);
}

}  // namespace

void RadioOutputScaleNode::init(Radio1938&, RadioInitContext&) {}

void RadioOutputScaleNode::reset(Radio1938&) {}

float RadioOutputScaleNode::run(Radio1938& radio,
                                float y,
                                RadioSampleContext&) {
  y = scalePhysicalSpeakerVoltsToDigital(radio, y);
  if (!radio.graph.isEnabled(PassId::Power)) return y;
  return y * std::max(radio.output.digitalMakeupGain, 0.0f);
}
