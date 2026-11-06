#include "power_supply.h"
#include <algorithm>

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
  supply.circuit.start(
      power.outputTubePlateSupplyVolts,
      {(single ? 1 : 2) * power.outputTubeQuiescentPlateCurrentAmps *
               (1 + supply.screenCurrentRatio) +
           supply.otherLoadAmps,
       (single ? 0 : power.tubeQuiescentPlateCurrentAmps) + supply.auxiliaryOtherLoadAmps,
       radio.receiverCircuit.tubeSupplyCurrentAmps});
}

void resetPowerSupply(Radio1938& radio) {
  radio.powerSupply.circuit.reset();
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
  radio.controlSense.powerSagSense =
      static_cast<float>(std::max(0.0, 1 - supply.circuit.volts(RectifierSupply::Main) /
                                               radio.power.outputTubePlateSupplyVolts));
}
