#ifndef RADIOIFY_AUDIOFILTER_RECEIVER_RECEIVER_INPUT_NETWORK_H
#define RADIOIFY_AUDIOFILTER_RECEIVER_RECEIVER_INPUT_NETWORK_H

#include "../../radio.h"

#include <array>

struct ReceiverInputNetworkSolve {
  float inputCurrent = 0.0f;
  float wiperVoltage = 0.0f;
  float tapVoltage = 0.0f;
  float gridVoltage = 0.0f;
  float couplingCapVoltage = 0.0f;
};

std::array<float, 2> computeReceiverControlDcNodes(
    const Radio1938::ReceiverCircuitNodeState& receiver,
    float sourceVoltage);

ReceiverInputNetworkSolve solveReceiverInputNetwork(
    const Radio1938::ReceiverCircuitNodeState& receiver,
    float dt,
    float sourceVoltage);

float computeReceiverDetectorLoadConductance(
    const Radio1938::ReceiverCircuitNodeState& receiver);

void commitReceiverInputNetworkSolve(
    Radio1938::ReceiverCircuitNodeState& receiver,
    const ReceiverInputNetworkSolve& solve);

#endif
