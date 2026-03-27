#include "../radio.h"
#include "receiver/receiver_input_network.h"

#include <algorithm>
#include <cassert>
#include <cmath>

float RadioDetectorLoadNode::process(Radio1938& radio,
                                     float y,
                                     const RadioSampleContext&) {
  auto& receiver = radio.receiverCircuit;
  if (!receiver.enabled) return y;

  float detectorNode = radio.demod.am.detectorNode;
  float dt = 1.0f / std::max(radio.sampleRate, 1.0f);

  if (receiver.warmStartPending) {
    auto dcNodes = computeReceiverControlDcNodes(receiver, detectorNode);
    receiver.volumeControlTapVoltage = dcNodes[1];
    receiver.couplingCapVoltage = dcNodes[0];
    receiver.gridVoltage = 0.0f;
    receiver.warmStartPending = false;
  }

  auto solve = solveReceiverInputNetwork(receiver, dt, detectorNode);
  commitReceiverInputNetworkSolve(receiver, solve);
  return y;
}
