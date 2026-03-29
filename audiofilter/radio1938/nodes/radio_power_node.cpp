#include "../../../radio.h"
#include "../../math/signal_math.h"
#include "../models/power_stage_solver.h"
#include "../models/power_supply.h"
#include "../../models/transformer_models.h"
#include "../../models/tube_models.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

float powerInternalOversampleFactor(const Radio1938& radio) {
  return std::clamp(radio.globals.oversampleFactor, 1.0f, 2.0f);
}

bool powerUsesInternalOversampling(const Radio1938& radio) {
  return powerInternalOversampleFactor(radio) > 1.0f;
}

float runPowerStageSample(Radio1938& radio, float y) {
  auto& power = radio.power;
  auto& controlSense = radio.controlSense;
  if (radio.calibration.enabled) {
    radio.calibration.validationSampleCount++;
  }
  float driverSupplyScale =
      computePowerBranchSupplyScale(radio, power.supplyDriveDepth);
  float outputSupplyScale = computePowerBranchSupplyScale(radio, 1.0f);
  float sampleRate = (power.internalSampleRate > 0.0f) ? power.internalSampleRate
                                                        : radio.sampleRate;
  float dt = 1.0f / requirePositiveFinite(sampleRate);
  float previousCapVoltage = power.gridCouplingCapVoltage;
  power.gridVoltage = solveCapCoupledGridVoltage(
      y, previousCapVoltage, dt, power.gridCouplingCapFarads,
      power.driverSourceResistanceOhms, power.gridLeakResistanceOhms,
      power.tubeBiasVolts, power.tubeGridCurrentResistanceOhms);
  float seriesCurrent =
      (y - previousCapVoltage - power.gridVoltage) /
      (power.driverSourceResistanceOhms +
       dt / requirePositiveFinite(power.gridCouplingCapFarads));
  power.gridCouplingCapVoltage +=
      dt * (seriesCurrent / requirePositiveFinite(power.gridCouplingCapFarads));
  float controlGridVolts = power.tubeBiasVolts + power.gridVoltage;
  if (radio.calibration.enabled) {
    radio.calibration.driverGridVolts.accumulate(power.gridVoltage);
  }
  if (radio.calibration.enabled && controlGridVolts > 0.0f) {
    radio.calibration.driverGridPositiveSamples++;
  }
  assert(power.tubeTriodeConnected &&
         "Interstage driver solve requires triode-connected 6F6 operation");
  float driverPlateQuiescent =
      requirePositiveFinite(power.tubeQuiescentPlateVolts * driverSupplyScale);
  float driverQuiescentCurrent = static_cast<float>(
      evaluateKorenTriodePlateRuntime(power.tubeBiasVolts,
                                      driverPlateQuiescent,
                                      power.tubeTriodeModel,
                                      power.tubeTriodeLut)
          .currentAmps);
  auto interstageSolved = solveDriverInterstageCenterTappedNoCap(
      power.interstageTransformer, power, controlGridVolts,
      driverPlateQuiescent, driverQuiescentCurrent);

  power.interstageCt.primaryCurrent = interstageSolved.primaryCurrent;
  power.interstageCt.primaryVoltage = interstageSolved.primaryVoltage;
  power.interstageCt.secondaryACurrent = interstageSolved.secondaryACurrent;
  power.interstageCt.secondaryAVoltage = interstageSolved.secondaryAVoltage;
  power.interstageCt.secondaryBCurrent = interstageSolved.secondaryBCurrent;
  power.interstageCt.secondaryBVoltage = interstageSolved.secondaryBVoltage;

  power.tubePlateVoltage =
      driverPlateQuiescent - interstageSolved.primaryVoltage;

  power.outputGridAVolts = interstageSolved.secondaryAVoltage;
  power.outputGridBVolts = interstageSolved.secondaryBVoltage;

  float actualDriverCurrent = interstageSolved.driverPlateCurrentAbs;

  if (radio.calibration.enabled) {
    radio.calibration.driverPlateSwingVolts.accumulate(
        interstageSolved.primaryVoltage);
    radio.calibration.outputGridAVolts.accumulate(power.outputGridAVolts);
    radio.calibration.outputGridBVolts.accumulate(power.outputGridBVolts);
    float interstageDifferentialVolts =
        interstageSolved.secondaryAVoltage - interstageSolved.secondaryBVoltage;

    radio.calibration.interstageSecondaryPeakVolts =
        std::max(radio.calibration.interstageSecondaryPeakVolts,
                 std::fabs(interstageDifferentialVolts));

    radio.calibration.interstageSecondarySumSq +=
        static_cast<double>(interstageDifferentialVolts) *
        static_cast<double>(interstageDifferentialVolts);
  }
  if (radio.calibration.enabled) {
    bool outputGridAPositive =
        (power.outputTubeBiasVolts + power.outputGridAVolts) > 0.0f;
    bool outputGridBPositive =
        (power.outputTubeBiasVolts + power.outputGridBVolts) > 0.0f;
    if (outputGridAPositive) radio.calibration.outputGridAPositiveSamples++;
    if (outputGridBPositive) radio.calibration.outputGridBPositiveSamples++;
    if (outputGridAPositive || outputGridBPositive) {
      radio.calibration.outputGridPositiveSamples++;
    }
  }
  float outputPlateQuiescent =
      requirePositiveFinite(power.outputTubeQuiescentPlateVolts *
                            outputSupplyScale);
  const float outputPrimaryLoadResistance = 0.0f;
  OutputStageSubstepResult outputSolved = runOutputStageSubsteps(
      power.outputTransformer, radio.speakerStage.speaker, power,
      outputPlateQuiescent, outputPrimaryLoadResistance);
  power.outputTransformer = outputSolved.transformer;
  float averagePrimaryVoltage = outputSolved.averagePrimaryVoltage;
  float averageSecondaryVoltage = outputSolved.averageSecondaryVoltage;
  float actualPlateCurrentA = outputSolved.averagePlateCurrentA;
  float actualPlateCurrentB = outputSolved.averagePlateCurrentB;
  y = averageSecondaryVoltage;
  if (power.postLpHz > 0.0f) {
    y = power.postLpf.process(y);
  }
  radio.speakerStage.physicalDriveVolts = y;
  if (radio.calibration.enabled) {
    radio.calibration.outputPrimaryVolts.accumulate(averagePrimaryVoltage);
    radio.calibration.speakerSecondaryVolts.accumulate(y);
    radio.calibration.maxDriverPlateCurrentAmps = std::max(
        radio.calibration.maxDriverPlateCurrentAmps, actualDriverCurrent);
    radio.calibration.maxOutputPlateCurrentAAmps = std::max(
        radio.calibration.maxOutputPlateCurrentAAmps, actualPlateCurrentA);
    radio.calibration.maxOutputPlateCurrentBAmps = std::max(
        radio.calibration.maxOutputPlateCurrentBAmps, actualPlateCurrentB);
    radio.calibration.maxSpeakerSecondaryVolts =
        std::max(radio.calibration.maxSpeakerSecondaryVolts, std::fabs(y));
    radio.calibration.maxSpeakerReferenceRatio = std::max(
        radio.calibration.maxSpeakerReferenceRatio,
        std::fabs(y) /
            std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f));
  }
  float quiescentSupplyCurrent =
      driverQuiescentCurrent + 2.0f * power.outputTubeQuiescentPlateCurrentAmps;
  float actualSupplyCurrent =
      actualDriverCurrent + actualPlateCurrentA + actualPlateCurrentB;
  float load = std::max(
      0.0f, (actualSupplyCurrent - quiescentSupplyCurrent) /
                requirePositiveFinite(quiescentSupplyCurrent));
  if (load > power.sagEnv) {
    power.sagEnv = power.sagAtk * power.sagEnv + (1.0f - power.sagAtk) * load;
  } else {
    power.sagEnv = power.sagRel * power.sagEnv + (1.0f - power.sagRel) * load;
  }
  controlSense.powerSagSense = power.sagEnv;
  advanceRectifierRipplePhase(radio);
  return y;
}

}  // namespace

void RadioPowerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& power = radio.power;
  power.internalSampleRate =
      radio.sampleRate * powerInternalOversampleFactor(radio);
  power.tubePlateDcVolts =
      power.tubePlateSupplyVolts -
      power.tubePlateCurrentAmps * power.interstagePrimaryResistanceOhms;
  power.outputTubePlateDcVolts =
      power.outputTubePlateSupplyVolts -
      power.outputTubePlateCurrentAmps *
          (0.5f * power.outputTransformerPrimaryResistanceOhms);
  assert(std::fabs(power.tubePlateSupplyVolts -
                   power.tubePlateCurrentAmps *
                       power.interstagePrimaryResistanceOhms -
                   power.tubePlateDcVolts) < 1.0f);
  assert(std::fabs(power.outputTubePlateSupplyVolts -
                   power.outputTubePlateCurrentAmps *
                       (0.5f * power.outputTransformerPrimaryResistanceOhms) -
                   power.outputTubePlateDcVolts) < 1.0f);
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  power.osLpIn.setLowpass(power.internalSampleRate, osCut, kRadioBiquadQ);
  power.osLpOut.setLowpass(power.internalSampleRate, osCut, kRadioBiquadQ);
  power.sagAtk =
      std::exp(-1.0f /
               (power.internalSampleRate * (power.sagAttackMs / 1000.0f)));
  power.sagRel =
      std::exp(-1.0f /
               (power.internalSampleRate * (power.sagReleaseMs / 1000.0f)));
  if (power.postLpHz > 0.0f) {
    power.postLpf.setLowpass(power.internalSampleRate, power.postLpHz,
                             kRadioBiquadQ);
  } else {
    power.postLpf = Biquad{};
  }
  power.driverSourceResistanceOhms = parallelResistance(
      radio.receiverCircuit.tubeLoadResistanceOhms,
      radio.receiverCircuit.tubePlateResistanceOhms);
  assert(std::isfinite(power.driverSourceResistanceOhms) &&
         power.driverSourceResistanceOhms > 0.0f);
  TriodeOperatingPoint driverOp = solveTriodeOperatingPoint(
      power.tubePlateSupplyVolts, power.interstagePrimaryResistanceOhms,
      power.tubeBiasVolts, power.tubePlateDcVolts, power.tubePlateCurrentAmps,
      power.tubeMutualConductanceSiemens, power.tubeMu);
  power.tubeQuiescentPlateVolts = driverOp.plateVolts;
  power.tubeQuiescentPlateCurrentAmps = driverOp.plateCurrentAmps;
  power.tubePlateResistanceOhms = driverOp.rpOhms;
  power.tubeTriodeModel = driverOp.model;
  configureRuntimeTriodeLut(power.tubeTriodeLut, power.tubeTriodeModel,
                            power.tubeBiasVolts, power.tubePlateSupplyVolts,
                            96.0f, 18.0f);
  assert((std::fabs(power.tubeQuiescentPlateVolts - power.tubePlateDcVolts) <=
          power.operatingPointToleranceVolts) &&
         "6F6 driver operating point diverged from the preset target");

  TriodeOperatingPoint outputOp = solveTriodeOperatingPoint(
      power.outputTubePlateSupplyVolts,
      0.5f * power.outputTransformerPrimaryResistanceOhms,
      power.outputTubeBiasVolts, power.outputTubePlateDcVolts,
      power.outputTubePlateCurrentAmps,
      power.outputTubeMutualConductanceSiemens, power.outputTubeMu);
  power.outputTubeQuiescentPlateVolts = outputOp.plateVolts;
  power.outputTubeQuiescentPlateCurrentAmps = outputOp.plateCurrentAmps;
  power.outputTubePlateResistanceOhms = outputOp.rpOhms;
  power.outputTubeTriodeModel = outputOp.model;
  configureRuntimeTriodeLut(power.outputTubeTriodeLut,
                            power.outputTubeTriodeModel,
                            power.outputTubeBiasVolts,
                            power.outputTubePlateSupplyVolts, 120.0f, 24.0f);
  assert((std::fabs(power.outputTubeQuiescentPlateVolts -
                    power.outputTubePlateDcVolts) <=
          power.outputOperatingPointToleranceVolts) &&
         "6B4 output operating point diverged from the preset target");

  power.tubePlateVoltage = power.tubeQuiescentPlateVolts;
  power.outputGridAVolts = 0.0f;
  power.outputGridBVolts = 0.0f;
  power.interstageTransformer.setModel(
      power.internalSampleRate, power.interstagePrimaryLeakageInductanceHenries,
      power.interstageMagnetizingInductanceHenries,
      power.interstageTurnsRatioPrimaryToSecondary,
      power.interstagePrimaryResistanceOhms,
      power.interstagePrimaryCoreLossResistanceOhms,
      power.interstagePrimaryShuntCapFarads,
      power.interstageSecondaryLeakageInductanceHenries,
      power.interstageSecondaryResistanceOhms,
      power.interstageSecondaryShuntCapFarads,
      power.interstageIntegrationSubsteps);
  power.outputTransformer.setModel(
      power.internalSampleRate,
      power.outputTransformerPrimaryLeakageInductanceHenries,
      power.outputTransformerMagnetizingInductanceHenries,
      power.outputTransformerTurnsRatioPrimaryToSecondary,
      power.outputTransformerPrimaryResistanceOhms,
      power.outputTransformerPrimaryCoreLossResistanceOhms,
      power.outputTransformerPrimaryShuntCapFarads,
      power.outputTransformerSecondaryLeakageInductanceHenries,
      power.outputTransformerSecondaryResistanceOhms,
      power.outputTransformerSecondaryShuntCapFarads,
      power.outputTransformerIntegrationSubsteps);
  {
    FixedLoadAffineTransformerProjection outputAffine =
        buildFixedLoadAffineProjection(power.outputTransformer,
                                       power.outputLoadResistanceOhms, 0.0f);
    power.outputTransformerAffineReady = true;
    power.outputTransformerAffineStateA = outputAffine.stateA;
    power.outputTransformerAffineSlope = outputAffine.slope;
  }
  // Derive the digital speaker reference from the modeled 6B4/output-transformer
  // combination instead of a hand-tuned watt scalar. This keeps the digital
  // scale anchored to the clean power the current tube/load model can actually
  // produce.
  power.nominalOutputPowerWatts = estimateOutputStageNominalPowerWatts(power);
  assert(std::isfinite(power.nominalOutputPowerWatts) &&
         power.nominalOutputPowerWatts > 0.0f);
  radio.output.digitalReferenceSpeakerVoltsPeak = std::sqrt(
      2.0f * power.nominalOutputPowerWatts * power.outputLoadResistanceOhms);
}

void RadioPowerNode::reset(Radio1938& radio) {
  auto& power = radio.power;
  power.sagEnv = 0.0f;
  power.rectifierPhase = 0.0f;
  power.osPrevInput = 0.0f;
  power.gridCouplingCapVoltage = 0.0f;
  power.gridVoltage = 0.0f;
  power.tubePlateVoltage = power.tubeQuiescentPlateVolts;
  power.outputGridAVolts = 0.0f;
  power.outputGridBVolts = 0.0f;
  power.interstageCt.primaryCurrent = 0.0f;
  power.interstageCt.primaryVoltage = 0.0f;
  power.interstageCt.secondaryACurrent = 0.0f;
  power.interstageCt.secondaryAVoltage = 0.0f;
  power.interstageCt.secondaryBCurrent = 0.0f;
  power.interstageCt.secondaryBVoltage = 0.0f;
  power.outputTransformer.clearState();
  power.osLpIn.reset();
  power.osLpOut.reset();
  power.postLpf.reset();
  radio.speakerStage.physicalDriveVolts = 0.0f;
}

float RadioPowerNode::run(Radio1938& radio, float y, RadioSampleContext&) {
  auto& power = radio.power;
  if (!powerUsesInternalOversampling(radio)) {
    return runPowerStageSample(radio, y);
  }
  return processOversampled2x(y, power.osPrevInput, power.osLpIn,
                              power.osLpOut, [&](float v) {
                                return runPowerStageSample(radio, v);
                              });
}
