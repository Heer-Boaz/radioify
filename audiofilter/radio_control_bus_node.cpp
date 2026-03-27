#include "../radio.h"

#include <algorithm>

void RadioControlBusNode::init(Radio1938&, RadioInitContext&) {}

void RadioControlBusNode::reset(Radio1938& radio) {
  radio.controlSense.reset();
  radio.controlBus.reset();
}

void RadioControlBusNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& controlBus = radio.controlBus;
  const auto& controlSense = radio.controlSense;
  const auto& power = radio.power;
  float supplyTarget =
      clampf((controlSense.powerSagSense - power.sagStart) /
                 std::max(1e-6f, power.sagEnd - power.sagStart),
             0.0f, 1.0f);
  controlBus.supplySag = supplyTarget;
}
