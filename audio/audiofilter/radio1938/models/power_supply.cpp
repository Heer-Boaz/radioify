#include "power_supply.h"
#include <algorithm>
#include <cmath>

void configurePowerSupply(Radio1938& radio) {
  auto& supply = radio.powerSupply;
  const auto& power = radio.power;
  const bool single =
      power.topology == Radio1938::PowerNodeState::Topology::CapCoupledSingleEnded;
  auto config = supply.config;
  config.audioResistanceOhms = radio.receiverCircuit.tubeDcFeedResistanceOhms;
  supply.circuit.configure(config, power.internalSampleRate,
                           radio.noiseConfig.humHzDefault);
  radio.receiverCircuit.tubeSupplyCurrentAmps =
      radio.receiverCircuit.tubeQuiescentPlateCurrentAmps;
  supply.mainReferenceVolts = power.outputTubePlateSupplyVolts;
  supply.circuit.start(
      power.outputTubePlateSupplyVolts,
      {(single ? 1 : 2) * power.outputTubeQuiescentPlateCurrentAmps *
               (1 + supply.screenCurrentRatio) +
           supply.otherLoadAmps,
       (single ? 0 : power.tubeQuiescentPlateCurrentAmps) + supply.auxiliaryOtherLoadAmps,
       radio.receiverCircuit.tubeSupplyCurrentAmps});
  // Unloaded one-pole sections: the screen dividers are far above each RC's
  // resistance at the ripple frequency.
  for (size_t k = 0; k < supply.screenFilterCoeff.size(); ++k) {
    const double tau = double(supply.screenFilterResistanceOhms[k]) * supply.screenFilterFarads[k];
    supply.screenFilterCoeff[k] =
        tau > 0 ? static_cast<float>(-std::expm1(-1.0 / (power.internalSampleRate * tau))) : 1.0f;
  }
  supply.screenSupplyVolts = static_cast<float>(supply.circuit.volts(RectifierSupply::Main));
  supply.screenFilterVolts.fill(supply.screenSupplyVolts);
}

void resetPowerSupply(Radio1938& radio) {
  radio.powerSupply.circuit.reset();
  auto& supply = radio.powerSupply;
  supply.screenSupplyVolts = static_cast<float>(supply.circuit.volts(RectifierSupply::Main));
  supply.screenFilterVolts.fill(supply.screenSupplyVolts);
  radio.receiverCircuit.tubeSupplyCurrentAmps =
      radio.receiverCircuit.tubeQuiescentPlateCurrentAmps;
}

void advancePowerSupply(Radio1938& radio, double outputPlateAmps,
                        double driverPlateAmps) {
  auto& supply = radio.powerSupply;
  supply.circuit.step(
      {outputPlateAmps * (1 + supply.screenCurrentRatio) + supply.otherLoadAmps,
       driverPlateAmps + supply.auxiliaryOtherLoadAmps,
       radio.receiverCircuit.tubeSupplyCurrentAmps});
  radio.receiverCircuit.tubeSupplyVolts =
      static_cast<float>(supply.circuit.volts(RectifierSupply::Audio));
  float volts = static_cast<float>(supply.circuit.volts(RectifierSupply::Main));
  for (size_t k = 0; k < supply.screenFilterVolts.size(); ++k) {
    supply.screenFilterVolts[k] += supply.screenFilterCoeff[k] * (volts - supply.screenFilterVolts[k]);
    volts = supply.screenFilterVolts[k];
  }
  supply.screenSupplyVolts = volts;
  radio.controlSense.powerSagSense =
      static_cast<float>(std::max(0.0, 1 - supply.circuit.volts(RectifierSupply::Main) /
                                               radio.power.outputTubePlateSupplyVolts));
}

float radioScreenSupplyScale(const Radio1938& radio) {
  const auto& supply = radio.powerSupply;
  if (!(supply.mainReferenceVolts > 0) || !(supply.screenSupplyVolts > 0)) return 1.0f;
  return supply.screenSupplyVolts / supply.mainReferenceVolts;
}
