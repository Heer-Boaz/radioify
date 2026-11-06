#include <algorithm>

#include "../../../radio.h"
#include "../models/receiver_input_network.h"

void RadioReceiverInputNetworkNode::init(Radio1938& radio, RadioInitContext&) {
  radio.receiverCircuit.detectorLoadConductance =
      static_cast<float>(receiverInputNetworkDc(radio.receiverCircuit).inputConductance);
}

void RadioReceiverInputNetworkNode::reset(Radio1938& radio) {
  auto& receiver = radio.receiverCircuit;
  receiver.detectorLoadConductance =
      static_cast<float>(receiverInputNetworkDc(receiver).inputConductance);
}

float RadioReceiverInputNetworkNode::run(Radio1938& radio, float y, RadioSampleContext&) {
  auto& receiver = radio.receiverCircuit;
  if (!receiver.enabled)
    return y;
  const double detectorVoltage = radio.demod.am.detectorNode;
  if (receiver.warmStartPending)
    primeReceiverInputNetwork(receiver, detectorVoltage);
  const auto step =
      receiverInputNetworkStep(receiver, 1 / std::max<double>(radio.sampleRate, 1));
  advanceReceiverInputNetwork(receiver, step, detectorVoltage);
  return y;
}
