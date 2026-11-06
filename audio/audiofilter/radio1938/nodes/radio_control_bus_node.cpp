#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>

void RadioControlBusNode::init(Radio1938&, RadioInitContext&) {}

void RadioControlBusNode::reset(Radio1938& radio) {
  radio.controlSense.reset();
  radio.controlBus.reset();
}

void RadioControlBusNode::run(Radio1938& radio, RadioSampleContext&) {
  auto& controlBus = radio.controlBus;
  const auto& controlSense = radio.controlSense;
  // Fractional B+ drop, measured by the supply network.
  controlBus.supplySag = clampf(controlSense.powerSagSense, 0.0f, 1.0f);
}
