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
  float receiverSupplyScale =
      computePowerBranchSupplyScale(radio, radio.power.supplyDriveDepth);
  assert(receiver.tubeTriodeConnected &&
         "Receiver audio stage expects a triode model");
  return processResistorLoadedTriodeStage(
      receiver.tubeBiasVolts + gridVoltage, receiverSupplyScale,
      receiver.tubePlateSupplyVolts, receiver.tubeQuiescentPlateVolts,
      receiver.tubeTriodeModel, &receiver.tubeTriodeLut,
      receiver.tubeLoadResistanceOhms, receiver.tubePlateVoltage);
}

}  // namespace

void RadioReceiverCircuitNode::init(Radio1938& radio, RadioInitContext&) {
  auto& receiver = radio.receiverCircuit;
  if (!receiver.enabled || !receiver.tubeTriodeConnected) return;
  float oversampleFactor = std::max(radio.globals.oversampleFactor, 1.0f);
  float osFs = radio.sampleRate * oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  receiver.osLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  receiver.osLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
  TriodeOperatingPoint op = solveTriodeOperatingPoint(
      receiver.tubePlateSupplyVolts, receiver.tubeLoadResistanceOhms,
      receiver.tubeBiasVolts, receiver.tubePlateDcVolts,
      receiver.tubePlateCurrentAmps, receiver.tubeMutualConductanceSiemens,
      receiver.tubeMu);
  receiver.tubeQuiescentPlateVolts = op.plateVolts;
  receiver.tubeQuiescentPlateCurrentAmps = op.plateCurrentAmps;
  receiver.tubePlateResistanceOhms = op.rpOhms;
  receiver.tubeTriodeModel = op.model;
  configureRuntimeTriodeLut(receiver.tubeTriodeLut, receiver.tubeTriodeModel,
                            receiver.tubeBiasVolts,
                            receiver.tubePlateSupplyVolts, 36.0f, 8.0f);
  assert((std::fabs(receiver.tubeQuiescentPlateVolts -
                    receiver.tubePlateDcVolts) <=
          receiver.operatingPointToleranceVolts) &&
         "6J5 first-audio operating point diverged from the preset target");
}

void RadioReceiverCircuitNode::reset(Radio1938& radio) {
  auto& receiver = radio.receiverCircuit;
  receiver.couplingCapVoltage = 0.0f;
  receiver.gridVoltage = 0.0f;
  receiver.volumeControlTapVoltage = 0.0f;
  receiver.warmStartPending = true;
  receiver.tubePlateVoltage = receiver.tubeQuiescentPlateVolts;
  receiver.osPrevGridVolts = 0.0f;
  receiver.osLpIn.reset();
  receiver.osLpOut.reset();
}

float RadioReceiverCircuitNode::run(Radio1938& radio,
                                    float y,
                                    RadioSampleContext&) {
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
  float out = 0.0f;
  float plateCurrent = 0.0f;
  out = processOversampled2x(receiver.gridVoltage, receiver.osPrevGridVolts,
                             receiver.osLpIn, receiver.osLpOut,
                             [&](float gridVoltage) {
                               return runReceiverTriodeStage(radio, gridVoltage);
                             });
  plateCurrent = static_cast<float>(
      evaluateKorenTriodePlateRuntime(receiver.tubeBiasVolts +
                                          receiver.gridVoltage,
                                      receiver.tubePlateVoltage,
                                      receiver.tubeTriodeModel,
                                      receiver.tubeTriodeLut)
          .currentAmps);
  if (radio.calibration.enabled) {
    radio.calibration.receiverPlateSwingVolts.accumulate(out);
    radio.calibration.maxReceiverPlateCurrentAmps =
        std::max(radio.calibration.maxReceiverPlateCurrentAmps, plateCurrent);
  }
  return out;
}
