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
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

bool powerUsesSingleEndedOutput(const Radio1938& radio) {
  return radio.power.topology ==
         Radio1938::PowerNodeState::Topology::CapCoupledSingleEnded;
}

// Thevenin source seen by the interstage coupling capacitor. A plate bypass
// capacitor is folded in with the same implicit step as the coupling network.
struct DriverSource {
  float volts;
  float ohms;
};

DriverSource driverPlateSource(const Radio1938& radio, float openCircuitVolts, float dt) {
  const auto& power = radio.power;
  const float cap = radio.receiverCircuit.tubePlateShuntCapFarads;
  if (!(cap > 0.0f)) return {openCircuitVolts, power.driverSourceResistanceOhms};
  const float sourceG = 1.0f / power.driverSourceResistanceOhms;
  const float capG = cap / dt;
  const float totalG = sourceG + capG;
  return {(sourceG * openCircuitVolts + capG * power.driverPlateCapVoltage) / totalG,
          1.0f / totalG};
}

// Coupling node N with grid leak, and a potentiometer from N to a capacitor
// whose wiper is the grid. All linear parts reduce to a Thevenin source at the
// wiper; only the positive-grid current is nonlinear. Backward Euler for both
// capacitors, as in solveCapCoupledGridVoltage.
float solvePotentiometerGrid(Radio1938& radio, const DriverSource& source, float dt,
                             float gridLeakOhms, float biasVolts, float gridCurrentOhms) {
  auto& power = radio.power;
  const double couplingCap = requirePositiveFinite(power.gridCouplingCapFarads);
  const double seriesTerm = double(source.ohms) + dt / couplingCap;
  const double sourceCurrent =
      (double(source.volts) - power.gridCouplingCapVoltage) / seriesTerm;
  const double nodeG = 1 / seriesTerm + 1 / double(requirePositiveFinite(gridLeakOhms));
  const double position = std::clamp<double>(power.gridPotentiometerPosition, 0, 1);
  const double upperOhms = (1 - position) * power.gridPotentiometerOhms;
  const double lowerOhms = position * power.gridPotentiometerOhms;
  const double capacitor = power.gridPotentiometerCapFarads;
  const double lowerG = capacitor > 0 ? 1 / (lowerOhms + dt / capacitor) : 0;
  // Thevenin at the wiper.
  const double nodeR = 1 / nodeG + upperOhms;
  const double wiperG = 1 / nodeR + lowerG;
  const double wiperOpenVolts =
      (sourceCurrent / nodeG / nodeR + lowerG * power.gridPotentiometerCapVoltage) / wiperG;
  const double gridCurrentG = 1 / double(requirePositiveFinite(gridCurrentOhms));
  double grid = wiperOpenVolts;
  for (int i = 0; i < 8; ++i) {
    const bool conducting = biasVolts + grid > 0;
    const double f = (wiperOpenVolts - grid) * wiperG -
                     (conducting ? (biasVolts + grid) * gridCurrentG : 0);
    const double df = -wiperG - (conducting ? gridCurrentG : 0);
    grid -= f / df;
  }
  const double node =
      upperOhms > 0 ? (sourceCurrent + grid / upperOhms) / (nodeG + 1 / upperOhms) : grid;
  const double seriesCurrent = (double(source.volts) - power.gridCouplingCapVoltage - node) /
                               seriesTerm;
  power.gridCouplingCapVoltage += static_cast<float>(dt * seriesCurrent / couplingCap);
  if (capacitor > 0) {
    const double lowerCurrent = lowerG * (grid - power.gridPotentiometerCapVoltage);
    power.gridPotentiometerCapVoltage += static_cast<float>(dt * lowerCurrent / capacitor);
  }
  power.driverPlateCapVoltage = static_cast<float>(source.volts - source.ohms * seriesCurrent);
  return static_cast<float>(grid);
}

float solveDriverCoupling(Radio1938& radio, float y, float dt, float gridLeakOhms,
                          float biasVolts, float gridCurrentOhms) {
  auto& power = radio.power;
  const DriverSource source = driverPlateSource(radio, y, dt);
  if (power.gridPotentiometerOhms > 0.0f)
    return solvePotentiometerGrid(radio, source, dt, gridLeakOhms, biasVolts,
                                  gridCurrentOhms);
  const float previousCapVoltage = power.gridCouplingCapVoltage;
  const float gridVoltage = solveCapCoupledGridVoltage(
      source.volts, previousCapVoltage, dt, power.gridCouplingCapFarads, source.ohms,
      gridLeakOhms, biasVolts, gridCurrentOhms);
  const float seriesCurrent =
      (source.volts - previousCapVoltage - gridVoltage) /
      (source.ohms + dt / requirePositiveFinite(power.gridCouplingCapFarads));
  power.gridCouplingCapVoltage +=
      dt * (seriesCurrent / requirePositiveFinite(power.gridCouplingCapFarads));
  power.driverPlateCapVoltage = source.volts - source.ohms * seriesCurrent;
  return gridVoltage;
}

float runSingleEndedPowerStageSample(Radio1938& radio, float y) {
  auto& power = radio.power;
  const bool calibrationEnabled = radio.calibration.enabled;
  if (calibrationEnabled) {
    radio.calibration.validationSampleCount++;
  }

  float sampleRate =
      (power.internalSampleRate > 0.0f) ? power.internalSampleRate : radio.sampleRate;
  float dt = 1.0f / requirePositiveFinite(sampleRate);
  power.gridVoltage =
      solveDriverCoupling(radio, y, dt, power.outputGridLeakResistanceOhms,
                          power.outputTubeBiasVolts, power.outputGridCurrentResistanceOhms);
  power.outputGridAVolts = power.gridVoltage;
  power.outputGridBVolts = 0.0f;

  if (calibrationEnabled) {
    radio.calibration.outputGridAVolts.accumulate(power.outputGridAVolts);
    radio.calibration.outputGridBVolts.accumulate(0.0f);
    bool outputGridPositive = (power.outputTubeBiasVolts + power.outputGridAVolts) > 0.0f;
    if (outputGridPositive) {
      radio.calibration.outputGridAPositiveSamples++;
      radio.calibration.outputGridPositiveSamples++;
    }
  }

  float outputPlateQuiescent = requirePositiveFinite(
      power.outputTubeQuiescentPlateVolts +
      static_cast<float>(radio.powerSupply.circuit.volts(RectifierSupply::Main)) -
      power.outputTubePlateSupplyVolts);
  constexpr float kOutputPrimaryLoadResistance = 0.0f;
  uint64_t outputStartNs = calibrationEnabled ? monotonicNowNs() : 0;
  OutputStageSubstepResult outputSolved = runSingleEndedOutputStageSubsteps(
      power.outputTransformer, radio.speakerStage.speaker, power, outputPlateQuiescent,
      power.outputGridAVolts, kOutputPrimaryLoadResistance);
  if (calibrationEnabled) {
    power.outputSolveTimeNs += monotonicNowNs() - outputStartNs;
    power.outputTransformerSubstepCount +=
        static_cast<uint64_t>(std::max(outputSolved.transformerSubsteps, 0));
    power.outputNewtonIterationCount +=
        static_cast<uint64_t>(std::max(outputSolved.totalNewtonIterations, 0));
    power.outputNewtonMaxIterations =
        std::max(power.outputNewtonMaxIterations,
                 static_cast<uint32_t>(std::max(outputSolved.maxNewtonIterations, 0)));
  }

  power.outputTransformer = outputSolved.transformer;
  float averagePrimaryVoltage = outputSolved.averagePrimaryVoltage;
  float actualPlateCurrent = outputSolved.averagePlateCurrentA;
  y = outputSolved.averageSecondaryVoltage;
  radio.speakerStage.physicalDriveVolts = y;

  if (calibrationEnabled) {
    radio.calibration.outputPrimaryVolts.accumulate(averagePrimaryVoltage);
    radio.calibration.speakerSecondaryVolts.accumulate(y);
    radio.calibration.maxOutputPlateCurrentAAmps =
        std::max(radio.calibration.maxOutputPlateCurrentAAmps, actualPlateCurrent);
    radio.calibration.maxSpeakerSecondaryVolts =
        std::max(radio.calibration.maxSpeakerSecondaryVolts, std::fabs(y));
    radio.calibration.maxSpeakerReferenceRatio = std::max(
        radio.calibration.maxSpeakerReferenceRatio,
        std::fabs(y) / std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f));
  }

  advancePowerSupply(radio, actualPlateCurrent, 0);
  return power.postLpHz > 0.0f
             ? power.postLpf.process(outputSolved.averageAcousticDriveVolts)
             : outputSolved.averageAcousticDriveVolts;
}

float runPowerStageSample(Radio1938& radio, float y) {
  if (powerUsesSingleEndedOutput(radio)) {
    return runSingleEndedPowerStageSample(radio, y);
  }
  auto& power = radio.power;
  const bool calibrationEnabled = radio.calibration.enabled;
  if (calibrationEnabled) {
    radio.calibration.validationSampleCount++;
  }

  float sampleRate =
      (power.internalSampleRate > 0.0f) ? power.internalSampleRate : radio.sampleRate;
  float dt = 1.0f / requirePositiveFinite(sampleRate);
  power.gridVoltage =
      solveDriverCoupling(radio, y, dt, power.gridLeakResistanceOhms, power.tubeBiasVolts,
                          power.tubeGridCurrentResistanceOhms);
  float controlGridVolts = power.tubeBiasVolts + power.gridVoltage;
  if (calibrationEnabled) {
    radio.calibration.driverGridVolts.accumulate(power.gridVoltage);
  }
  if (calibrationEnabled && controlGridVolts > 0.0f) {
    radio.calibration.driverGridPositiveSamples++;
  }
  assert(power.tubeTriodeConnected &&
         "Interstage driver solve requires triode-connected 6F6 operation");
  float driverPlateQuiescent = requirePositiveFinite(
      power.tubeQuiescentPlateVolts +
      static_cast<float>(radio.powerSupply.circuit.volts(RectifierSupply::Auxiliary)) -
      power.tubePlateSupplyVolts);
  // The transformer stores deviations from the fixed DC operating point.
  // Recentring on every B+ sample would cancel supply-induced plate current.
  const float driverQuiescentCurrent = power.tubeQuiescentPlateCurrentAmps;
  uint64_t interstageStartNs = calibrationEnabled ? monotonicNowNs() : 0;
  auto interstageSolved = solveDriverInterstageCenterTappedNoCap(
      power.interstageTransformer, power, controlGridVolts, driverPlateQuiescent,
      driverQuiescentCurrent);
  if (calibrationEnabled) {
    power.interstageSolveTimeNs += monotonicNowNs() - interstageStartNs;
    power.interstageSubstepCount +=
        static_cast<uint64_t>(std::max(interstageSolved.substeps, 0));
    power.interstageIterationCount +=
        static_cast<uint64_t>(std::max(interstageSolved.totalIterations, 0));
    power.interstageMaxIterations =
        std::max(power.interstageMaxIterations,
                 static_cast<uint32_t>(std::max(interstageSolved.maxIterations, 0)));
    power.interstageDriverEvalCount += interstageSolved.driverEvalCount;
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
  power.interstageAdaptiveSubsteps = std::max(interstageSolved.suggestedSubsteps, 1);
  power.interstageAdaptiveValidationCountdown =
      std::max(interstageSolved.suggestedValidationCountdown, 0);

  power.interstageCt.primaryCurrent = interstageSolved.primaryCurrent;
  power.interstageCt.primaryVoltage = interstageSolved.primaryVoltage;
  power.interstageCt.secondaryACurrent = interstageSolved.secondaryACurrent;
  power.interstageCt.secondaryAVoltage = interstageSolved.secondaryAVoltage;
  power.interstageCt.secondaryBCurrent = interstageSolved.secondaryBCurrent;
  power.interstageCt.secondaryBVoltage = interstageSolved.secondaryBVoltage;

  power.tubePlateVoltage = driverPlateQuiescent - interstageSolved.primaryVoltage;

  power.outputGridAVolts = interstageSolved.secondaryAVoltage;
  power.outputGridBVolts = interstageSolved.secondaryBVoltage;

  float actualDriverCurrent = interstageSolved.driverPlateCurrentAbs;

  if (calibrationEnabled) {
    radio.calibration.driverPlateSwingVolts.accumulate(interstageSolved.primaryVoltage);
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
  float outputPlateQuiescent = requirePositiveFinite(
      power.outputTubeQuiescentPlateVolts +
      static_cast<float>(radio.powerSupply.circuit.volts(RectifierSupply::Main)) -
      power.outputTubePlateSupplyVolts);
  const float outputPrimaryLoadResistance = 0.0f;
  uint64_t outputStartNs = calibrationEnabled ? monotonicNowNs() : 0;
  OutputStageSubstepResult outputSolved =
      runOutputStageSubsteps(power.outputTransformer, radio.speakerStage.speaker, power,
                             outputPlateQuiescent, outputPrimaryLoadResistance);
  if (calibrationEnabled) {
    power.outputSolveTimeNs += monotonicNowNs() - outputStartNs;
    power.outputTransformerSubstepCount +=
        static_cast<uint64_t>(std::max(outputSolved.transformerSubsteps, 0));
    power.outputNewtonIterationCount +=
        static_cast<uint64_t>(std::max(outputSolved.totalNewtonIterations, 0));
    power.outputNewtonMaxIterations =
        std::max(power.outputNewtonMaxIterations,
                 static_cast<uint32_t>(std::max(outputSolved.maxNewtonIterations, 0)));
  }
  power.outputTransformer = outputSolved.transformer;
  float averagePrimaryVoltage = outputSolved.averagePrimaryVoltage;
  float averageSecondaryVoltage = outputSolved.averageSecondaryVoltage;
  float actualPlateCurrentA = outputSolved.averagePlateCurrentA;
  float actualPlateCurrentB = outputSolved.averagePlateCurrentB;
  y = averageSecondaryVoltage;
  radio.speakerStage.physicalDriveVolts = y;
  if (calibrationEnabled) {
    radio.calibration.outputPrimaryVolts.accumulate(averagePrimaryVoltage);
    radio.calibration.speakerSecondaryVolts.accumulate(y);
    radio.calibration.maxDriverPlateCurrentAmps =
        std::max(radio.calibration.maxDriverPlateCurrentAmps, actualDriverCurrent);
    radio.calibration.maxOutputPlateCurrentAAmps =
        std::max(radio.calibration.maxOutputPlateCurrentAAmps, actualPlateCurrentA);
    radio.calibration.maxOutputPlateCurrentBAmps =
        std::max(radio.calibration.maxOutputPlateCurrentBAmps, actualPlateCurrentB);
    radio.calibration.maxSpeakerSecondaryVolts =
        std::max(radio.calibration.maxSpeakerSecondaryVolts, std::fabs(y));
    radio.calibration.maxSpeakerReferenceRatio = std::max(
        radio.calibration.maxSpeakerReferenceRatio,
        std::fabs(y) / std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f));
  }
  advancePowerSupply(radio, actualPlateCurrentA + actualPlateCurrentB,
                     actualDriverCurrent);
  // The acoustic node receives pressure-equivalent acceleration; electrical
  // voltage, current and cone motion remain owned by this coupled solve.
  return power.postLpHz > 0.0f
             ? power.postLpf.process(outputSolved.averageAcousticDriveVolts)
             : outputSolved.averageAcousticDriveVolts;
}

}  // namespace

void RadioPowerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& power = radio.power;
  power.oversampler.configure(radio.globals.oversampleFactor,
                              radio.globals.oversampleCutoffFraction);
  power.terminalResampler.configure(radio.globals.oversampleFactor,
                                    radio.globals.oversampleCutoffFraction);
  power.internalSampleRate = radio.sampleRate * power.oversampler.factor();
  const bool singleEnded = powerUsesSingleEndedOutput(radio);
  float outputPrimaryDcResistance =
      power.outputTransformerPrimaryResistanceOhms * (singleEnded ? 1.0f : 0.5f);
  power.outputTubePlateDcVolts =
      power.outputTubePlateSupplyVolts -
      power.outputTubePlateCurrentAmps * outputPrimaryDcResistance;
  assert(std::fabs(power.outputTubePlateSupplyVolts -
                   power.outputTubePlateCurrentAmps * outputPrimaryDcResistance -
                   power.outputTubePlateDcVolts) < 1.0f);
  if (power.postLpHz > 0.0f) {
    power.postLpf.setLowpass(power.internalSampleRate, power.postLpHz, kRadioBiquadQ);
  } else {
    power.postLpf = Biquad{};
  }
  power.driverSourceResistanceOhms =
      parallelResistance(radio.receiverCircuit.tubeLoadResistanceOhms,
                         radio.receiverCircuit.tubePlateResistanceOhms);
  assert(std::isfinite(power.driverSourceResistanceOhms) &&
         power.driverSourceResistanceOhms > 0.0f);
  if (singleEnded) {
    PentodeOperatingPoint outputOp = solvePentodeOperatingPoint(
        power.outputTubePlateSupplyVolts, power.outputTubeScreenVolts,
        outputPrimaryDcResistance, power.outputTubeBiasVolts,
        power.outputTubePlateDcVolts, power.outputTubePlateCurrentAmps,
        power.outputTubeMutualConductanceSiemens, power.outputTubePlateKneeVolts,
        power.outputTubeGridSoftnessVolts, power.outputTubeCutoffVolts);
    power.outputTubeQuiescentPlateVolts = outputOp.plateVolts;
    power.outputTubeQuiescentPlateCurrentAmps = outputOp.plateCurrentAmps;
    power.outputTubePlateResistanceOhms = outputOp.rpOhms;
    power.outputTubePentodeCurrentForGrid = prepareFixedPlatePentodeEvaluator(
        outputOp.plateVolts, power.outputTubeScreenVolts, power.outputTubeBiasVolts,
        power.outputTubeCutoffVolts, outputOp.plateVolts, power.outputTubeScreenVolts,
        outputOp.plateCurrentAmps, power.outputTubeMutualConductanceSiemens,
        power.outputTubePlateKneeVolts, power.outputTubeGridSoftnessVolts);
    assert(
        (std::fabs(power.outputTubeQuiescentPlateVolts - power.outputTubePlateDcVolts) <=
         power.outputOperatingPointToleranceVolts) &&
        "41 output operating point diverged from the preset target");
    power.tubePlateVoltage = 0.0f;
    power.interstageTransformer.clearState();
  } else {
    power.tubePlateDcVolts =
        power.tubePlateSupplyVolts -
        power.tubePlateCurrentAmps * power.interstagePrimaryResistanceOhms;
    assert(std::fabs(power.tubePlateSupplyVolts -
                     power.tubePlateCurrentAmps * power.interstagePrimaryResistanceOhms -
                     power.tubePlateDcVolts) < 1.0f);
    TriodeOperatingPoint driverOp = solveTriodeOperatingPoint(
        power.tubePlateSupplyVolts, power.interstagePrimaryResistanceOhms,
        power.tubeBiasVolts, power.tubePlateDcVolts, power.tubePlateCurrentAmps,
        power.tubeMutualConductanceSiemens, power.tubeMu);
    power.tubeQuiescentPlateVolts = driverOp.plateVolts;
    power.tubeQuiescentPlateCurrentAmps = driverOp.plateCurrentAmps;
    power.tubePlateResistanceOhms = driverOp.rpOhms;
    power.tubeTriodeModel = driverOp.model;
    configureRuntimeTriodeLut(power.tubeTriodeLut, power.tubeTriodeModel,
                              power.tubeBiasVolts, power.tubePlateSupplyVolts, 96.0f,
                              18.0f);
    assert((std::fabs(power.tubeQuiescentPlateVolts - power.tubePlateDcVolts) <=
            power.operatingPointToleranceVolts) &&
           "6F6 driver operating point diverged from the preset target");

    TriodeOperatingPoint outputOp = solveTriodeOperatingPoint(
        power.outputTubePlateSupplyVolts, outputPrimaryDcResistance,
        power.outputTubeBiasVolts, power.outputTubePlateDcVolts,
        power.outputTubePlateCurrentAmps, power.outputTubeMutualConductanceSiemens,
        power.outputTubeMu);
    power.outputTubeQuiescentPlateVolts = outputOp.plateVolts;
    power.outputTubeQuiescentPlateCurrentAmps = outputOp.plateCurrentAmps;
    power.outputTubePlateResistanceOhms = outputOp.rpOhms;
    power.outputTubeTriodeModel = outputOp.model;
    configureRuntimeTriodeLut(power.outputTubeTriodeLut, power.outputTubeTriodeModel,
                              power.outputTubeBiasVolts, power.outputTubePlateSupplyVolts,
                              120.0f, 24.0f);
    assert(
        (std::fabs(power.outputTubeQuiescentPlateVolts - power.outputTubePlateDcVolts) <=
         power.outputOperatingPointToleranceVolts) &&
        "6B4 output operating point diverged from the preset target");

    power.tubePlateVoltage = power.tubeQuiescentPlateVolts;
    power.interstageTransformer.setModel(
        power.internalSampleRate, power.interstagePrimaryLeakageInductanceHenries,
        power.interstageMagnetizingInductanceHenries,
        power.interstageTurnsRatioPrimaryToSecondary,
        power.interstagePrimaryResistanceOhms,
        power.interstagePrimaryCoreLossResistanceOhms,
        power.interstagePrimaryShuntCapFarads,
        power.interstageSecondaryLeakageInductanceHenries,
        power.interstageSecondaryResistanceOhms, power.interstageSecondaryShuntCapFarads,
        power.interstageIntegrationSubsteps);
  }
  power.outputGridAVolts = 0.0f;
  power.outputGridBVolts = 0.0f;
  power.outputTransformer.setModel(
      power.internalSampleRate, power.outputTransformerPrimaryLeakageInductanceHenries,
      power.outputTransformerMagnetizingInductanceHenries,
      power.outputTransformerTurnsRatioPrimaryToSecondary,
      power.outputTransformerPrimaryResistanceOhms,
      power.outputTransformerPrimaryCoreLossResistanceOhms,
      power.outputTransformerPrimaryShuntCapFarads,
      power.outputTransformerSecondaryLeakageInductanceHenries,
      power.outputTransformerSecondaryResistanceOhms,
      power.outputTransformerSecondaryShuntCapFarads,
      power.outputTransformerIntegrationSubsteps);
  configureSpeakerElectromechanics(radio.speakerStage.speaker, power.internalSampleRate,
                                   power.outputLoadResistanceOhms);
  // Derive the digital speaker reference from the active tube/transformer
  // topology instead of a hand-tuned gain scalar.
  power.nominalOutputPowerWatts =
      singleEnded ? estimateSingleEndedOutputStageNominalPowerWatts(power)
                  : estimateOutputStageNominalPowerWatts(power);
  assert(std::isfinite(power.nominalOutputPowerWatts) &&
         power.nominalOutputPowerWatts > 0.0f);
  configurePowerSupply(radio);
  radio.output.digitalReferenceSpeakerVoltsPeak =
      std::sqrt(2.0f * power.nominalOutputPowerWatts * power.outputLoadResistanceOhms);
}

void RadioPowerNode::reset(Radio1938& radio) {
  auto& power = radio.power;
  resetPowerSupply(radio);
  power.gridCouplingCapVoltage = 0.0f;
  power.driverPlateCapVoltage = 0.0f;
  power.gridPotentiometerCapVoltage = 0.0f;
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
  power.interstageAdaptiveValidationAttemptCount = 0;
  power.interstageAdaptiveAcceptedDownshiftCount = 0;
  power.interstageAdaptiveBoundaryErrorSumVolts = 0.0;
  power.interstageAdaptiveBoundaryErrorMaxVolts = 0.0f;
  power.outputSolveTimeNs = 0;
  power.interstageAdaptiveSubsteps = 0;
  power.interstageAdaptiveValidationCountdown = 0;
  power.outputTransformer.clearState();
  resetSpeakerElectromechanics(radio.speakerStage.speaker);
  power.oversampler.reset();
  power.terminalResampler.reset();
  power.postLpf.reset();
  radio.speakerStage.physicalDriveVolts = 0.0f;
}

float RadioPowerNode::run(Radio1938& radio, float y, RadioSampleContext&) {
  auto& power = radio.power;
  std::array<float, Oversampler::kMaxFactor> terminal{};
  int sample = 0;
  const float out = power.oversampler.process(y, [&](float v) {
    const float acoustic = runPowerStageSample(radio, v);
    terminal[sample++] = radio.speakerStage.physicalDriveVolts;
    return acoustic;
  });
  radio.speakerStage.physicalDriveVolts = power.terminalResampler.downsample(terminal);
  return out;
}
