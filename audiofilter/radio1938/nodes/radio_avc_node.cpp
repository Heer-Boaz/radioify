#include "../../../radio.h"
#include "../../math/signal_math.h"

void RadioAVCNode::init(Radio1938&, RadioInitContext&) {}

void RadioAVCNode::reset(Radio1938&) {}

void RadioAVCNode::run(Radio1938& radio, RadioSampleContext&) {
  auto& controlBus = radio.controlBus;
  const auto& controlSense = radio.controlSense;
  controlBus.controlVoltage =
      clampf(controlSense.controlVoltageSense, 0.0f, 1.25f);
}
