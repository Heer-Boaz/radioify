#include "receiver_input_network.h"

#include <algorithm>
#include <cassert>

namespace {
ReceiverInputNetworkStep makeStep(const Radio1938::ReceiverCircuitNodeState& receiver,
                                  double dt) {
  if (!receiver.enabled)
    return {};
  const double resistance = receiver.volumeControlResistanceOhms;
  assert(resistance > 0);
  const double w = std::clamp<double>(receiver.volumeControlPosition, 0, 1);
  const double t =
      std::clamp(receiver.volumeControlTapResistanceOhms / resistance, 0.0, 1.0);
  const double blend = t > 0 ? std::clamp((t - w) / t, 0.0, 1.0) : 0;
  const double loudnessG = receiver.volumeControlLoudnessResistanceOhms > 0
                               ? blend / receiver.volumeControlLoudnessResistanceOhms
                               : 0;
  const double tapCapG =
      dt > 0 ? 2 * blend * receiver.volumeControlLoudnessCapFarads / dt : 0;
  const double tapG = loudnessG + tapCapG;
  const double gridG =
      dt > 0 && receiver.couplingCapFarads > 0
          ? 1 / (receiver.gridLeakResistanceOhms + dt / (2 * receiver.couplingCapFarads))
          : 0;

  // Exact Thevenin resistance matrix of a tapped potentiometer. Zero-length
  // segments (ground, full volume, or wiper on tap) remain exact connections.
  // Unlike a tiny-resistor stamp, this cannot drown out megohm conductances.
  const double zww = resistance * w * (1 - w);
  const double ztt = resistance * t * (1 - t);
  const double zwt = resistance * std::min(w, t) * (1 - std::max(w, t));
  const double a = 1 + zww * gridG;
  const double b = zwt * tapG;
  const double c = zwt * gridG;
  const double d = 1 + ztt * tapG;
  // Expanded determinant avoids subtracting nearly equal loaded terms.
  const double det =
      1 + zww * gridG + ztt * tapG + (zww * ztt - zwt * zwt) * gridG * tapG;
  const double gridHistoryVoltage =
      receiver.couplingCapVoltage +
      (gridG > 0 ? dt / (2 * receiver.couplingCapFarads) * receiver.couplingCapCurrent
                 : 0);
  const double gridHistory = gridG * gridHistoryVoltage;
  const double tapHistory = tapCapG > 0 ? tapCapG * receiver.volumeControlTapVoltage +
                                              receiver.volumeControlTapCapCurrent
                                        : 0;
  const double h1 = zww * gridHistory + zwt * tapHistory;
  const double h2 = zwt * gridHistory + ztt * tapHistory;
  ReceiverInputNetworkStep step;
  step.wiperGain = (d * w - b * t) / det;
  step.tapGain = (a * t - c * w) / det;
  step.wiperHistory = (d * h1 - b * h2) / det;
  step.tapHistory = (a * h2 - c * h1) / det;
  step.gridConductance = gridG;
  step.gridHistoryVoltage = gridHistoryVoltage;
  step.tapCapConductance = tapCapG;
  // Reciprocity gives the source current: V/R + w*I_grid + t*I_tap.
  step.inputConductance =
      1 / resistance + w * gridG * step.wiperGain + t * tapG * step.tapGain;
  step.historyCurrent = w * (gridG * step.wiperHistory - gridHistory) +
                        t * (tapG * step.tapHistory - tapHistory);
  return step;
}
}  // namespace

ReceiverInputNetworkStep receiverInputNetworkStep(
    const Radio1938::ReceiverCircuitNodeState& receiver, double dt) {
  assert(dt > 0);
  return makeStep(receiver, dt);
}

ReceiverInputNetworkStep receiverInputNetworkDc(
    const Radio1938::ReceiverCircuitNodeState& receiver) {
  return makeStep(receiver, 0);
}

void primeReceiverInputNetwork(Radio1938::ReceiverCircuitNodeState& receiver,
                               double detectorVoltage) {
  const auto dc = receiverInputNetworkDc(receiver);
  receiver.couplingCapVoltage = static_cast<float>(dc.wiperGain * detectorVoltage);
  receiver.volumeControlTapVoltage = static_cast<float>(dc.tapGain * detectorVoltage);
  receiver.gridVoltage = 0;
  receiver.couplingCapCurrent = 0;
  receiver.volumeControlTapCapCurrent = 0;
  receiver.warmStartPending = false;
}

void advanceReceiverInputNetwork(Radio1938::ReceiverCircuitNodeState& receiver,
                                 const ReceiverInputNetworkStep& step,
                                 double detectorVoltage) {
  const double wiper = step.wiperGain * detectorVoltage + step.wiperHistory;
  const double tap = step.tapGain * detectorVoltage + step.tapHistory;
  const double gridCurrent = step.gridConductance * (wiper - step.gridHistoryVoltage);
  const double grid = receiver.gridLeakResistanceOhms * gridCurrent;
  receiver.couplingCapCurrent = static_cast<float>(gridCurrent);
  receiver.volumeControlTapCapCurrent =
      step.tapCapConductance > 0
          ? static_cast<float>(step.tapCapConductance *
                                   (tap - receiver.volumeControlTapVoltage) -
                               receiver.volumeControlTapCapCurrent)
          : 0;
  receiver.gridVoltage = static_cast<float>(grid);
  receiver.couplingCapVoltage = static_cast<float>(wiper - grid);
  receiver.volumeControlTapVoltage = static_cast<float>(tap);
  receiver.warmStartPending = false;
}
