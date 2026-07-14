#include "../../../radio.h"
#include "../../math/signal_math.h"
#include "../models/power_stage_solver.h"
#include "../models/power_supply.h"
#include "../../models/transformer_models.h"
#include "../../models/tube_models.h"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>

namespace {

uint64_t monotonicNowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

float powerInternalOversampleFactor(const Radio1938& radio) {
  return std::clamp(radio.globals.oversampleFactor, 1.0f, 2.0f);
}

bool powerUsesInternalOversampling(const Radio1938& radio) {
  return powerInternalOversampleFactor(radio) > 1.0f;
}

bool powerUsesSingleEndedOutput(const Radio1938& radio) {
  return radio.power.topology ==
         Radio1938::PowerNodeState::Topology::CapCoupledSingleEnded;
}

float runSingleEndedPowerStageSample(Radio1938& radio, float y) {
  auto& power = radio.power;
  auto& controlSense = radio.controlSense;
  const bool calibrationEnabled = radio.calibration.enabled;
  if (calibrationEnabled) {
    radio.calibration.validationSampleCount++;
  }

  float outputSupplyScale = computePowerBranchSupplyScale(radio, 1.0f);
  float sampleRate = (power.internalSampleRate > 0.0f) ? power.internalSampleRate
                                                        : radio.sampleRate;
  float dt = 1.0f / requirePositiveFinite(sampleRate);
  float previousCapVoltage = power.gridCouplingCapVoltage;
  power.gridVoltage = solveCapCoupledGridVoltage(
      y, previousCapVoltage, dt, power.gridCouplingCapFarads,
      power.driverSourceResistanceOhms, power.outputGridLeakResistanceOhms,
      power.outputTubeBiasVolts, power.outputGridCurrentResistanceOhms);
  float seriesCurrent =
      (y - previousCapVoltage - power.gridVoltage) /
      (power.driverSourceResistanceOhms +
       dt / requirePositiveFinite(power.gridCouplingCapFarads));
  power.gridCouplingCapVoltage +=
      dt * (seriesCurrent / requirePositiveFinite(power.gridCouplingCapFarads));
  power.outputGridAVolts = power.gridVoltage;
  power.outputGridBVolts = 0.0f;

  if (calibrationEnabled) {
    radio.calibration.outputGridAVolts.accumulate(power.outputGridAVolts);
    radio.calibration.outputGridBVolts.accumulate(0.0f);
    bool outputGridPositive =
        (power.outputTubeBiasVolts + power.outputGridAVolts) > 0.0f;
    if (outputGridPositive) {
      radio.calibration.outputGridAPositiveSamples++;
      radio.calibration.outputGridPositiveSamples++;
    }
  }

  float outputPlateQuiescent =
      requirePositiveFinite(power.outputTubeQuiescentPlateVolts *
                            outputSupplyScale);
  constexpr float kOutputPrimaryLoadResistance = 0.0f;
  uint64_t outputStartNs = calibrationEnabled ? monotonicNowNs() : 0;
  OutputStageSubstepResult outputSolved = runSingleEndedOutputStageSubsteps(
      power.outputTransformer, radio.speakerStage.speaker, power,
      outputPlateQuiescent, power.outputGridAVolts,
      kOutputPrimaryLoadResistance);
  if (calibrationEnabled) {
    power.outputSolveTimeNs += monotonicNowNs() - outputStartNs;
    power.outputTransformerSubstepCount +=
        static_cast<uint64_t>(std::max(outputSolved.transformerSubsteps, 0));
    power.outputNewtonIterationCount +=
        static_cast<uint64_t>(std::max(outputSolved.totalNewtonIterations, 0));
    power.outputNewtonMaxIterations = std::max(
        power.outputNewtonMaxIterations,
        static_cast<uint32_t>(std::max(outputSolved.maxNewtonIterations, 0)));
  }

  power.outputTransformer = outputSolved.transformer;
  float averagePrimaryVoltage = outputSolved.averagePrimaryVoltage;
  float actualPlateCurrent = outputSolved.averagePlateCurrentA;
  y = outputSolved.averageSecondaryVoltage;
  if (power.postLpHz > 0.0f) {
    y = power.postLpf.process(y);
  }
  radio.speakerStage.physicalDriveVolts = y;

  if (calibrationEnabled) {
    radio.calibration.outputPrimaryVolts.accumulate(averagePrimaryVoltage);
    radio.calibration.speakerSecondaryVolts.accumulate(y);
    radio.calibration.maxOutputPlateCurrentAAmps = std::max(
        radio.calibration.maxOutputPlateCurrentAAmps, actualPlateCurrent);
    radio.calibration.maxSpeakerSecondaryVolts =
        std::max(radio.calibration.maxSpeakerSecondaryVolts, std::fabs(y));
    radio.calibration.maxSpeakerReferenceRatio = std::max(
        radio.calibration.maxSpeakerReferenceRatio,
        std::fabs(y) /
            std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f));
  }

  float quiescentSupplyCurrent =
      requirePositiveFinite(power.outputTubeQuiescentPlateCurrentAmps);
  float load = std::max(
      0.0f, (actualPlateCurrent - quiescentSupplyCurrent) /
                quiescentSupplyCurrent);
  if (load > power.sagEnv) {
    power.sagEnv = power.sagAtk * power.sagEnv + (1.0f - power.sagAtk) * load;
  } else {
    power.sagEnv = power.sagRel * power.sagEnv + (1.0f - power.sagRel) * load;
  }
  controlSense.powerSagSense = power.sagEnv;
  advanceRectifierRipplePhase(radio);
  return radio.speakerStage.speaker.electricalCurrentAmps *
         radio.speakerStage.speaker.effectiveVoiceCoilResistanceOhms;
}

float runPowerStageSample(Radio1938& radio, float y) {
  if (powerUsesSingleEndedOutput(radio)) {
    return runSingleEndedPowerStageSample(radio, y);
  }
  auto& power = radio.power;
  auto& controlSense = radio.controlSense;
  const bool calibrationEnabled = radio.calibration.enabled;
  if (calibrationEnabled) {
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
  if (calibrationEnabled) {
    radio.calibration.driverGridVolts.accumulate(power.gridVoltage);
  }
  if (calibrationEnabled && controlGridVolts > 0.0f) {
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
  uint64_t interstageStartNs = calibrationEnabled ? monotonicNowNs() : 0;
  auto interstageSolved = solveDriverInterstageCenterTappedNoCap(
      power.interstageTransformer, power, controlGridVolts,
      driverPlateQuiescent, driverQuiescentCurrent, calibrationEnabled);
  if (calibrationEnabled) {
    power.interstageSolveTimeNs += monotonicNowNs() - interstageStartNs;
    power.interstageSubstepCount +=
        static_cast<uint64_t>(std::max(interstageSolved.substeps, 0));
    power.interstageIterationCount +=
        static_cast<uint64_t>(std::max(interstageSolved.totalIterations, 0));
    power.interstageMaxIterations = std::max(
        power.interstageMaxIterations,
        static_cast<uint32_t>(std::max(interstageSolved.maxIterations, 0)));
    power.interstageDriverEvalCount += interstageSolved.driverEvalCount;
    power.interstageDriverEvalTimeNs += interstageSolved.driverEvalTimeNs;
    power.interstageAdaptiveValidationAttemptCount +=
        interstageSolved.adaptiveValidationAttempts;
    power.interstageAdaptiveAcceptedDownshiftCount +=
        interstageSolved.adaptiveAcceptedDownshifts;
    power.interstageAdaptiveBoundaryErrorSumVolts +=
        static_cast<double>(interstageSolved.adaptiveBoundaryErrorVolts);
    power.interstageAdaptiveBoundaryErrorMaxVolts =
        std::max(power.interstageAdaptiveBoundaryErrorMaxVolts,
                 interstageSolved.adaptiveBoundaryErrorVolts);
  }
  power.interstageAdaptiveSubsteps =
      std::max(interstageSolved.suggestedSubsteps, 1);
  power.interstageAdaptiveValidationCountdown =
      std::max(interstageSolved.suggestedValidationCountdown, 0);

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

  if (calibrationEnabled) {
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
  if (calibrationEnabled) {
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
  uint64_t outputStartNs = calibrationEnabled ? monotonicNowNs() : 0;
  OutputStageSubstepResult outputSolved = runOutputStageSubsteps(
      power.outputTransformer, radio.speakerStage.speaker, power,
      outputPlateQuiescent, outputPrimaryLoadResistance);
  if (calibrationEnabled) {
    power.outputSolveTimeNs += monotonicNowNs() - outputStartNs;
    power.outputTransformerSubstepCount +=
        static_cast<uint64_t>(std::max(outputSolved.transformerSubsteps, 0));
    power.outputNewtonIterationCount +=
        static_cast<uint64_t>(std::max(outputSolved.totalNewtonIterations, 0));
    power.outputNewtonMaxIterations = std::max(
        power.outputNewtonMaxIterations,
        static_cast<uint32_t>(std::max(outputSolved.maxNewtonIterations, 0)));
  }
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
  if (calibrationEnabled) {
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
  // Publish an explicit motor-equivalent drive to the acoustic speaker node;
  // the electrical load remains owned and advanced by this power-stage solve.
  return radio.speakerStage.speaker.electricalCurrentAmps *
         radio.speakerStage.speaker.effectiveVoiceCoilResistanceOhms;
}

}  // namespace

void RadioPowerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& power = radio.power;
  power.internalSampleRate =
      radio.sampleRate * powerInternalOversampleFactor(radio);
  const bool singleEnded = powerUsesSingleEndedOutput(radio);
  float outputPrimaryDcResistance =
      power.outputTransformerPrimaryResistanceOhms *
      (singleEnded ? 1.0f : 0.5f);
  power.outputTubePlateDcVolts =
      power.outputTubePlateSupplyVolts -
      power.outputTubePlateCurrentAmps * outputPrimaryDcResistance;
  assert(std::fabs(power.outputTubePlateSupplyVolts -
                   power.outputTubePlateCurrentAmps *
                       outputPrimaryDcResistance -
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
  configureRectifierRipple(radio);
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
  if (singleEnded) {
    PentodeOperatingPoint outputOp = solvePentodeOperatingPoint(
        power.outputTubePlateSupplyVolts, power.outputTubeScreenVolts,
        outputPrimaryDcResistance, power.outputTubeBiasVolts,
        power.outputTubePlateDcVolts, power.outputTubePlateCurrentAmps,
        power.outputTubeMutualConductanceSiemens,
        power.outputTubePlateKneeVolts, power.outputTubeGridSoftnessVolts,
        power.outputTubeCutoffVolts);
    power.outputTubeQuiescentPlateVolts = outputOp.plateVolts;
    power.outputTubeQuiescentPlateCurrentAmps = outputOp.plateCurrentAmps;
    power.outputTubePlateResistanceOhms = outputOp.rpOhms;
    power.outputTubePentodeCurrentForGrid =
        prepareFixedPlatePentodeEvaluator(
            outputOp.plateVolts, power.outputTubeScreenVolts,
            power.outputTubeBiasVolts, power.outputTubeCutoffVolts,
            outputOp.plateVolts, power.outputTubeScreenVolts,
            outputOp.plateCurrentAmps,
            power.outputTubeMutualConductanceSiemens,
            power.outputTubePlateKneeVolts,
            power.outputTubeGridSoftnessVolts);
    assert((std::fabs(power.outputTubeQuiescentPlateVolts -
                      power.outputTubePlateDcVolts) <=
            power.outputOperatingPointToleranceVolts) &&
           "41 output operating point diverged from the preset target");
    power.tubePlateVoltage = 0.0f;
    power.interstageTransformer.clearState();
  } else {
    power.tubePlateDcVolts =
        power.tubePlateSupplyVolts -
        power.tubePlateCurrentAmps * power.interstagePrimaryResistanceOhms;
    assert(std::fabs(power.tubePlateSupplyVolts -
                     power.tubePlateCurrentAmps *
                         power.interstagePrimaryResistanceOhms -
                     power.tubePlateDcVolts) < 1.0f);
    TriodeOperatingPoint driverOp = solveTriodeOperatingPoint(
        power.tubePlateSupplyVolts, power.interstagePrimaryResistanceOhms,
        power.tubeBiasVolts, power.tubePlateDcVolts,
        power.tubePlateCurrentAmps, power.tubeMutualConductanceSiemens,
        power.tubeMu);
    power.tubeQuiescentPlateVolts = driverOp.plateVolts;
    power.tubeQuiescentPlateCurrentAmps = driverOp.plateCurrentAmps;
    power.tubePlateResistanceOhms = driverOp.rpOhms;
    power.tubeTriodeModel = driverOp.model;
    configureRuntimeTriodeLut(power.tubeTriodeLut, power.tubeTriodeModel,
                              power.tubeBiasVolts,
                              power.tubePlateSupplyVolts, 96.0f, 18.0f);
    assert((std::fabs(power.tubeQuiescentPlateVolts -
                      power.tubePlateDcVolts) <=
            power.operatingPointToleranceVolts) &&
           "6F6 driver operating point diverged from the preset target");

    TriodeOperatingPoint outputOp = solveTriodeOperatingPoint(
        power.outputTubePlateSupplyVolts, outputPrimaryDcResistance,
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
    power.interstageTransformer.setModel(
        power.internalSampleRate,
        power.interstagePrimaryLeakageInductanceHenries,
        power.interstageMagnetizingInductanceHenries,
        power.interstageTurnsRatioPrimaryToSecondary,
        power.interstagePrimaryResistanceOhms,
        power.interstagePrimaryCoreLossResistanceOhms,
        power.interstagePrimaryShuntCapFarads,
        power.interstageSecondaryLeakageInductanceHenries,
        power.interstageSecondaryResistanceOhms,
        power.interstageSecondaryShuntCapFarads,
        power.interstageIntegrationSubsteps);
  }
  power.outputGridAVolts = 0.0f;
  power.outputGridBVolts = 0.0f;
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
  configureSpeakerElectromechanics(radio.speakerStage.speaker,
                                   power.internalSampleRate,
                                   power.outputLoadResistanceOhms);
  {
    FixedLoadAffineTransformerProjection outputAffine =
        buildFixedLoadAffineProjection(power.outputTransformer,
                                       power.outputLoadResistanceOhms, 0.0f);
    power.outputTransformerAffineReady = true;
    power.outputTransformerAffineStateA = outputAffine.stateA;
    power.outputTransformerAffineSlope = outputAffine.slope;
  }
  // Derive the digital speaker reference from the active tube/transformer
  // topology instead of a hand-tuned gain scalar.
  power.nominalOutputPowerWatts =
      singleEnded ? estimateSingleEndedOutputStageNominalPowerWatts(power)
                  : estimateOutputStageNominalPowerWatts(power);
  assert(std::isfinite(power.nominalOutputPowerWatts) &&
         power.nominalOutputPowerWatts > 0.0f);
  radio.output.digitalReferenceSpeakerVoltsPeak = std::sqrt(
      2.0f * power.nominalOutputPowerWatts * power.outputLoadResistanceOhms);
}

void RadioPowerNode::reset(Radio1938& radio) {
  auto& power = radio.power;
  power.sagEnv = 0.0f;
  resetRectifierRipple(radio);
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
  power.outputTransformerSubstepCount = 0;
  power.outputNewtonIterationCount = 0;
  power.outputNewtonMaxIterations = 0;
  power.interstageSubstepCount = 0;
  power.interstageIterationCount = 0;
  power.interstageMaxIterations = 0;
  power.interstageSolveTimeNs = 0;
  power.interstageDriverEvalCount = 0;
  power.interstageDriverEvalTimeNs = 0;
  power.interstageAdaptiveValidationAttemptCount = 0;
  power.interstageAdaptiveAcceptedDownshiftCount = 0;
  power.interstageAdaptiveBoundaryErrorSumVolts = 0.0;
  power.interstageAdaptiveBoundaryErrorMaxVolts = 0.0f;
  power.outputSolveTimeNs = 0;
  power.interstageAdaptiveSubsteps = 0;
  power.interstageAdaptiveValidationCountdown = 0;
  power.outputTransformer.clearState();
  resetSpeakerElectromechanics(radio.speakerStage.speaker);
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
