#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_RECEIVER_INPUT_NETWORK_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_RECEIVER_INPUT_NETWORK_H

#include "../../../radio.h"

// Trapezoidal companion of the pot, loudness branch and grid coupling cap.
// The port current is G * detectorVoltage + historyCurrent. The detector uses
// this same port that the input node commits, so the two stages exchange charge
// rather than approximating the load by an unrelated parallel resistor.
struct ReceiverInputNetworkStep {
  double inputConductance = 0;
  double historyCurrent = 0;
  double wiperGain = 0;
  double wiperHistory = 0;
  double tapGain = 0;
  double tapHistory = 0;
  double gridConductance = 0;
  double gridHistoryVoltage = 0;
  double tapConductance = 0;
  double tapHistoryVoltage = 0;
};

ReceiverInputNetworkStep receiverInputNetworkStep(
    const Radio1938::ReceiverCircuitNodeState& receiver, double dt);
ReceiverInputNetworkStep receiverInputNetworkDc(
    const Radio1938::ReceiverCircuitNodeState& receiver);
void primeReceiverInputNetwork(Radio1938::ReceiverCircuitNodeState& receiver,
                               double detectorVoltage);
void advanceReceiverInputNetwork(Radio1938::ReceiverCircuitNodeState& receiver,
                                 const ReceiverInputNetworkStep& step,
                                 double detectorVoltage);

#endif
