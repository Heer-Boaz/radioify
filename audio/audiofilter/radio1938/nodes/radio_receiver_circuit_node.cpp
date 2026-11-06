#include "../../../radio.h"
#include "../models/power_supply.h"
#include "../../math/signal_math.h"
#include "../../models/tube_models.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

float runReceiverTriodeStage(Radio1938& radio, float gridVoltage) {
  auto& receiver = radio.receiverCircuit;
  const float rail = receiver.tubeSupplyVolts;
  assert(receiver.tubeTriodeConnected && "Receiver audio stage expects a triode model");
  const float output = processResistorLoadedTriodeStage(
      receiver.tubeBiasVolts + gridVoltage, 1.0f, rail, receiver.tubeQuiescentPlateVolts,
      receiver.tubeTriodeModel, &receiver.tubeTriodeLut, receiver.tubeLoadResistanceOhms,
      receiver.tubePlateVoltage);
  receiver.tubeSupplyCurrentAmps +=
      (rail - receiver.tubePlateVoltage) / receiver.tubeLoadResistanceOhms;
  return output;
}

}  // namespace

void RadioReceiverCircuitNode::init(Radio1938& radio, RadioInitContext&) {
  auto& receiver = radio.receiverCircuit;
  if (!receiver.enabled || !receiver.tubeTriodeConnected) return;
  receiver.oversampler.configure(radio.globals.oversampleFactor,
                                 radio.globals.oversampleCutoffFraction);
  receiver.tubeTriodeModel.mu = receiver.tubeMu;
  TriodeOperatingPoint op = solveTriodeOperatingPoint(
      receiver.tubePlateSupplyVolts,
      receiver.tubeLoadResistanceOhms + receiver.tubeDcFeedResistanceOhms,
      receiver.tubeBiasVolts, receiver.tubePlateDcVolts, receiver.tubeTriodeModel);
  // The bypassed feed resistor belongs to the DC path; it is not the plate's
  // audio-frequency load. Reservoir dynamics remain in the supply model.
  receiver.tubeEffectiveSupplyVolts =
      receiver.tubePlateSupplyVolts -
      op.plateCurrentAmps * receiver.tubeDcFeedResistanceOhms;
  receiver.tubeSupplyVolts = receiver.tubeEffectiveSupplyVolts;
  receiver.tubeSupplyCurrentAmps = op.plateCurrentAmps;
  receiver.tubeQuiescentPlateVolts = op.plateVolts;
  receiver.tubeQuiescentPlateCurrentAmps = op.plateCurrentAmps;
  receiver.tubePlateResistanceOhms = op.rpOhms;
  receiver.tubeTriodeModel = op.model;
  configureRuntimeTriodeLut(receiver.tubeTriodeLut, receiver.tubeTriodeModel,
                            receiver.tubeBiasVolts, receiver.tubePlateSupplyVolts, 36.0f,
                            8.0f);
  assert((std::fabs(receiver.tubeQuiescentPlateVolts - receiver.tubePlateDcVolts) <=
          receiver.operatingPointToleranceVolts) &&
         "First-audio operating point diverged from the circuit prior");
}

void RadioReceiverCircuitNode::reset(Radio1938& radio) {
  auto& receiver = radio.receiverCircuit;
  receiver.couplingCapVoltage = 0.0f;
  receiver.couplingCapCurrent = 0.0f;
  receiver.volumeControlTapCapCurrent = 0.0f;
  receiver.volumeControlTapCapVoltage = 0.0f;
  receiver.gridVoltage = 0.0f;
  receiver.volumeControlTapVoltage = 0.0f;
  receiver.warmStartPending = true;
  receiver.tubePlateVoltage = receiver.tubeQuiescentPlateVolts;
  receiver.tubeSupplyVolts = receiver.tubeEffectiveSupplyVolts;
  receiver.tubeSupplyCurrentAmps = receiver.tubeQuiescentPlateCurrentAmps;
  receiver.oversampler.reset();
}

float RadioReceiverCircuitNode::run(Radio1938& radio, float y, RadioSampleContext&) {
  auto& receiver = radio.receiverCircuit;
  if (!receiver.enabled) return y;
  assert(!receiver.warmStartPending &&
         "ReceiverInputNetwork node must run before ReceiverCircuit");
  if (receiver.warmStartPending) return 0.0f;
  assert(std::isfinite(receiver.couplingCapVoltage) &&
         std::isfinite(receiver.gridVoltage) &&
         std::isfinite(receiver.volumeControlTapVoltage));
  if (radio.calibration.enabled) {
    radio.calibration.receiverGridVolts.accumulate(receiver.gridVoltage);
  }
  receiver.tubeSupplyCurrentAmps = 0;
  const float out = receiver.oversampler.process(
      receiver.gridVoltage,
      [&](float gridVoltage) { return runReceiverTriodeStage(radio, gridVoltage); });
  receiver.tubeSupplyCurrentAmps /= receiver.oversampler.factor();
  if (radio.calibration.enabled) {
    float plateCurrent = static_cast<float>(
        evaluateKorenTriodePlateRuntime(receiver.tubeBiasVolts + receiver.gridVoltage,
                                        receiver.tubePlateVoltage,
                                        receiver.tubeTriodeModel, receiver.tubeTriodeLut)
            .currentAmps);
    radio.calibration.receiverPlateSwingVolts.accumulate(out);
    radio.calibration.maxReceiverPlateCurrentAmps =
        std::max(radio.calibration.maxReceiverPlateCurrentAmps, plateCurrent);
  }
  return out;
}
