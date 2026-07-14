#include "power_stage_solver.h"
#include "power_stage_internal.h"

#include "../../math/signal_math.h"
#include "../../models/tube_models.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace power_stage_internal {

TubeGridBranchEval evaluateTubeGridBranch(float acGridVolts,
                                          float biasVolts,
                                          float gridLeakConductance,
                                          float gridCurrentConductance) {
  float slope = gridLeakConductance;
  float current = acGridVolts * gridLeakConductance;
  float conductingVolts = biasVolts + acGridVolts;
  if (conductingVolts > 0.0f) {
    current += conductingVolts * gridCurrentConductance;
    slope += gridCurrentConductance;
  }
  TubeGridBranchEval result{};
  result.current = current;
  result.slope = slope;
  return result;
}

TriodeLutView makeTriodeLutView(const KorenTriodeLut& lut) {
  TriodeLutView view{};
  if (!lut.valid()) {
    return view;
  }
  view.currentAmps = lut.currentAmps.data();
  view.conductanceSiemens = lut.conductanceSiemens.data();
  view.vgkMin = lut.vgkMin;
  view.vgkMax = lut.vgkMax;
  view.vpkMin = lut.vpkMin;
  view.vpkMax = lut.vpkMax;
  view.vgkInvStep = lut.vgkInvStep;
  view.vpkInvStep = lut.vpkInvStep;
  view.vgkBins = lut.vgkBins;
  view.vpkBins = lut.vpkBins;
  view.valid = true;
  return view;
}

KorenTriodePlateEval evaluateTriodePlateFast(float vgk,
                                             float vpk,
                                             const KorenTriodeModel& model,
                                             const KorenTriodeLut& lut,
                                             const TriodeLutView& view) {
  if (!view.valid) {
    return evaluateKorenTriodePlateRuntime(vgk, vpk, model, lut);
  }

  float clampedVgk = std::clamp(vgk, view.vgkMin, view.vgkMax);
  float clampedVpk = std::clamp(vpk, view.vpkMin, view.vpkMax);
  float x = (clampedVgk - view.vgkMin) * view.vgkInvStep;
  float y = (clampedVpk - view.vpkMin) * view.vpkInvStep;
  int x0 = std::clamp(static_cast<int>(x), 0, view.vgkBins - 2);
  int y0 = std::clamp(static_cast<int>(y), 0, view.vpkBins - 2);
  float tx = x - static_cast<float>(x0);
  float ty = y - static_cast<float>(y0);
  int x1 = x0 + 1;
  int y1 = y0 + 1;

  size_t row0 = static_cast<size_t>(y0) * static_cast<size_t>(view.vgkBins);
  size_t row1 = static_cast<size_t>(y1) * static_cast<size_t>(view.vgkBins);
  float i00 = view.currentAmps[row0 + static_cast<size_t>(x0)];
  float i10 = view.currentAmps[row0 + static_cast<size_t>(x1)];
  float i01 = view.currentAmps[row1 + static_cast<size_t>(x0)];
  float i11 = view.currentAmps[row1 + static_cast<size_t>(x1)];
  float g00 = view.conductanceSiemens[row0 + static_cast<size_t>(x0)];
  float g10 = view.conductanceSiemens[row0 + static_cast<size_t>(x1)];
  float g01 = view.conductanceSiemens[row1 + static_cast<size_t>(x0)];
  float g11 = view.conductanceSiemens[row1 + static_cast<size_t>(x1)];

  float i0 = i00 + (i10 - i00) * tx;
  float i1 = i01 + (i11 - i01) * tx;
  float g0 = g00 + (g10 - g00) * tx;
  float g1 = g01 + (g11 - g01) * tx;

  KorenTriodePlateEval eval{};
  eval.currentAmps = i0 + (i1 - i0) * ty;
  eval.conductanceSiemens = g0 + (g1 - g0) * ty;
  return eval;
}

OutputTubePairEval evaluateOutputTubePair(
    const Radio1938::PowerNodeState& power,
    const TriodeLutView& outputTriodeLut,
    float outputPlateQuiescent,
    float gridA,
    float gridB,
    float primaryVoltage) {
  float plateA = outputPlateQuiescent - 0.5f * primaryVoltage;
  float plateB = outputPlateQuiescent + 0.5f * primaryVoltage;
  KorenTriodePlateEval evalA = evaluateTriodePlateFast(
      power.outputTubeBiasVolts + gridA, plateA, power.outputTubeTriodeModel,
      power.outputTubeTriodeLut, outputTriodeLut);
  KorenTriodePlateEval evalB = evaluateTriodePlateFast(
      power.outputTubeBiasVolts + gridB, plateB, power.outputTubeTriodeModel,
      power.outputTubeTriodeLut, outputTriodeLut);

  OutputTubePairEval result{};
  result.plateCurrentA = static_cast<float>(evalA.currentAmps);
  result.plateCurrentB = static_cast<float>(evalB.currentAmps);
  result.driveCurrent = 0.5f * (result.plateCurrentA - result.plateCurrentB);
  result.driveSlope = -0.25f * static_cast<float>(evalA.conductanceSiemens +
                                                  evalB.conductanceSiemens);
  return result;
}

}  // namespace power_stage_internal

namespace {

float deriveSpeakerCompliance(float movingMassKg, float suspensionHz) {
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return 1.0f / std::max(movingMassKg * w0 * w0, 1e-9f);
}

float deriveSpeakerMechanicalDamping(float movingMassKg,
                                     float suspensionHz,
                                     float suspensionQ) {
  float q = std::max(suspensionQ, 0.35f);
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return (movingMassKg * w0) / q;
}

float deriveSpeakerVoiceCoilInductance(float voiceCoilResistanceOhms,
                                       float topLpHz) {
  float lpHz = std::max(topLpHz, 2500.0f);
  return voiceCoilResistanceOhms / (kRadioTwoPi * lpHz);
}

float deriveSpeakerForceFactor(float movingMassKg,
                               float suspensionHz,
                               float voiceCoilResistanceOhms) {
  constexpr float kElectricalQ = 0.95f;
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return std::sqrt(std::max(w0 * movingMassKg * voiceCoilResistanceOhms /
                                kElectricalQ,
                            1e-9f));
}

float deriveSpeakerMechanicalDampingFromQ(float movingMassKg,
                                          float suspensionHz,
                                          float mechanicalQ) {
  float q = std::max(mechanicalQ, 0.35f);
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return (movingMassKg * w0) / q;
}

float deriveSpeakerForceFactorFromQ(float movingMassKg,
                                    float suspensionHz,
                                    float voiceCoilResistanceOhms,
                                    float electricalQ) {
  float q = std::max(electricalQ, 0.35f);
  float w0 = kRadioTwoPi * std::max(suspensionHz, 1.0f);
  return std::sqrt(std::max(w0 * movingMassKg * voiceCoilResistanceOhms / q,
                            1e-9f));
}

OutputPrimarySolveResult solveOutputPrimaryAffineWithLut(
    const AffineTransformerProjection& projection,
    const Radio1938::PowerNodeState& power,
    const power_stage_internal::TriodeLutView& outputTriodeLut,
    float outputPlateQuiescent,
    float gridA,
    float gridB,
    float initialPrimaryVoltage) {
  float primaryVoltage = initialPrimaryVoltage;
  constexpr float kPrimaryVoltageTolerance = 5e-5f;
  int iterations = 0;

  for (int iter = 0; iter < 8; ++iter) {
    iterations = iter + 1;
    power_stage_internal::OutputTubePairEval pair =
        power_stage_internal::evaluateOutputTubePair(
            power, outputTriodeLut, outputPlateQuiescent, gridA, gridB,
            primaryVoltage);

    float f = projection.base.primaryVoltage +
              projection.slope.primaryVoltage * pair.driveCurrent -
              primaryVoltage;
    float df = projection.slope.primaryVoltage * pair.driveSlope - 1.0f;
    assert(std::isfinite(df) && std::fabs(df) >= 1e-9f);

    float deltaPrimaryVoltage = f / df;
    primaryVoltage -= deltaPrimaryVoltage;
    assert(std::isfinite(primaryVoltage));
    if (std::fabs(deltaPrimaryVoltage) < kPrimaryVoltageTolerance) break;
  }

  power_stage_internal::OutputTubePairEval finalPair =
      power_stage_internal::evaluateOutputTubePair(
          power, outputTriodeLut, outputPlateQuiescent, gridA, gridB,
          primaryVoltage);
  OutputPrimarySolveResult result{};
  result.primaryVoltage = primaryVoltage;
  result.driveCurrent = finalPair.driveCurrent;
  result.plateCurrentA = finalPair.plateCurrentA;
  result.plateCurrentB = finalPair.plateCurrentB;
  result.iterations = iterations;
  return result;
}

}  // namespace

OutputPrimarySolveResult solveOutputPrimaryAffine(
    const AffineTransformerProjection& projection,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float gridA,
    float gridB,
    float initialPrimaryVoltage) {
  const power_stage_internal::TriodeLutView outputTriodeLut =
      power_stage_internal::makeTriodeLutView(power.outputTubeTriodeLut);
  return solveOutputPrimaryAffineWithLut(projection, power, outputTriodeLut,
                                         outputPlateQuiescent, gridA, gridB,
                                         initialPrimaryVoltage);
}

namespace {

struct SingleEndedPrimarySolveResult {
  float primaryVoltage = 0.0f;
  float driveCurrent = 0.0f;
  float plateCurrent = 0.0f;
  int iterations = 0;
};

float evaluateSingleEndedPlateCurrent(
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float outputGridVolts,
    float primaryVoltage) {
  float gridControlledCurrent =
      power.outputTubePentodeCurrentForGrid.eval(
          power.outputTubeBiasVolts + outputGridVolts);
  float plateVoltage = std::max(outputPlateQuiescent - primaryVoltage, 0.0f);
  float plateFeedbackCurrent =
      (plateVoltage - power.outputTubeQuiescentPlateVolts) /
      requirePositiveFinite(power.outputTubePlateResistanceOhms);
  return std::max(gridControlledCurrent + plateFeedbackCurrent, 0.0f);
}

SingleEndedPrimarySolveResult solveSingleEndedPrimaryAffine(
    const AffineTransformerProjection& projection,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float outputGridVolts,
    float initialPrimaryVoltage) {
  float primaryVoltage = initialPrimaryVoltage;
  constexpr float kPrimaryVoltageTolerance = 5e-5f;
  int iterations = 0;

  for (int iter = 0; iter < 4; ++iter) {
    iterations = iter + 1;
    float plateCurrent = evaluateSingleEndedPlateCurrent(
        power, outputPlateQuiescent, outputGridVolts, primaryVoltage);
    float driveCurrent =
        plateCurrent - power.outputTubeQuiescentPlateCurrentAmps;
    float f = projection.base.primaryVoltage +
              projection.slope.primaryVoltage * driveCurrent - primaryVoltage;
    bool plateConducting = plateCurrent > 0.0f &&
                           outputPlateQuiescent - primaryVoltage > 0.0f;
    float driveSlope =
        plateConducting
            ? -1.0f /
                  requirePositiveFinite(power.outputTubePlateResistanceOhms)
            : 0.0f;
    float df = projection.slope.primaryVoltage * driveSlope - 1.0f;
    assert(std::isfinite(df) && std::fabs(df) >= 1e-9f);
    float deltaPrimaryVoltage = f / df;
    primaryVoltage -= deltaPrimaryVoltage;
    assert(std::isfinite(primaryVoltage));
    if (std::fabs(deltaPrimaryVoltage) < kPrimaryVoltageTolerance) break;
  }

  float plateCurrent = evaluateSingleEndedPlateCurrent(
      power, outputPlateQuiescent, outputGridVolts, primaryVoltage);
  SingleEndedPrimarySolveResult result{};
  result.primaryVoltage = primaryVoltage;
  result.driveCurrent =
      plateCurrent - power.outputTubeQuiescentPlateCurrentAmps;
  result.plateCurrent = plateCurrent;
  result.iterations = iterations;
  return result;
}

}  // namespace

void configureSpeakerElectromechanics(SpeakerSim& speaker,
                                      float sampleRate,
                                      float nominalLoadOhms) {
  speaker.electricalSampleRate = sampleRate;
  speaker.nominalLoadOhms = std::max(nominalLoadOhms, 1e-3f);
  speaker.effectiveVoiceCoilResistanceOhms =
      speaker.voiceCoilResistanceOhms > 0.0f
          ? speaker.voiceCoilResistanceOhms
          : 0.82f * speaker.nominalLoadOhms;
  speaker.effectiveMovingMassKg =
      speaker.movingMassKg > 0.0f ? speaker.movingMassKg : 0.014f;
  speaker.effectiveSuspensionComplianceMetersPerNewton =
      speaker.suspensionComplianceMetersPerNewton > 0.0f
          ? speaker.suspensionComplianceMetersPerNewton
          : deriveSpeakerCompliance(speaker.effectiveMovingMassKg,
                                    speaker.suspensionHz);
  if (speaker.mechanicalDampingNsPerMeter > 0.0f) {
    speaker.effectiveMechanicalDampingNsPerMeter =
        speaker.mechanicalDampingNsPerMeter;
  } else if (speaker.mechanicalQ > 0.0f) {
    speaker.effectiveMechanicalDampingNsPerMeter =
        deriveSpeakerMechanicalDampingFromQ(speaker.effectiveMovingMassKg,
                                            speaker.suspensionHz,
                                            speaker.mechanicalQ);
  } else {
    speaker.effectiveMechanicalDampingNsPerMeter =
        deriveSpeakerMechanicalDamping(speaker.effectiveMovingMassKg,
                                       speaker.suspensionHz,
                                       speaker.suspensionQ);
  }
  speaker.effectiveVoiceCoilInductanceHenries =
      speaker.voiceCoilInductanceHenries > 0.0f
          ? speaker.voiceCoilInductanceHenries
          : deriveSpeakerVoiceCoilInductance(
                speaker.effectiveVoiceCoilResistanceOhms, speaker.topLpHz);
  if (speaker.forceFactorBl > 0.0f) {
    speaker.effectiveForceFactorBl = speaker.forceFactorBl;
  } else if (speaker.electricalQ > 0.0f) {
    speaker.effectiveForceFactorBl = deriveSpeakerForceFactorFromQ(
        speaker.effectiveMovingMassKg, speaker.suspensionHz,
        speaker.effectiveVoiceCoilResistanceOhms, speaker.electricalQ);
  } else {
    speaker.effectiveForceFactorBl =
        deriveSpeakerForceFactor(speaker.effectiveMovingMassKg,
                                 speaker.suspensionHz,
                                 speaker.effectiveVoiceCoilResistanceOhms);
  }
  constexpr float kLoadSenseMs = 1.0f;
  speaker.loadSenseCoeff =
      std::exp(-1.0f /
               (std::max(sampleRate, 1.0f) * (kLoadSenseMs / 1000.0f)));
  speaker.effectiveLoadOhms = speaker.nominalLoadOhms;
}

void resetSpeakerElectromechanics(SpeakerSim& speaker) {
  speaker.electricalCurrentAmps = 0.0f;
  speaker.coneVelocityMetersPerSecond = 0.0f;
  speaker.coneDisplacementMeters = 0.0f;
  speaker.backEmfVolts = 0.0f;
  speaker.loadSenseVoltage = 0.0f;
  speaker.loadSenseCurrent = 0.0f;
  speaker.effectiveLoadOhms = std::max(speaker.nominalLoadOhms, 1e-3f);
}

SpeakerElectricalLinearization linearizeSpeakerElectricalLoad(
    const SpeakerSim& speaker,
    float nominalLoadOhms,
    float dt) {
  SpeakerElectricalLinearization linearization{};
  float nominalLoad = requirePositiveFinite(nominalLoadOhms);
  float re = std::max(speaker.effectiveVoiceCoilResistanceOhms, 1e-4f);
  float le = std::max(speaker.effectiveVoiceCoilInductanceHenries, 1e-7f);
  float mass = std::max(speaker.effectiveMovingMassKg, 1e-6f);
  float compliance =
      std::max(speaker.effectiveSuspensionComplianceMetersPerNewton, 1e-8f);
  float damping = std::max(speaker.effectiveMechanicalDampingNsPerMeter, 1e-5f);
  float bl = std::max(speaker.effectiveForceFactorBl, 1e-4f);
  float safeDt = std::max(dt, 1e-9f);
  float a = le / safeDt + re;
  float d = mass / safeDt + damping + safeDt / compliance;
  float det = a * d + bl * bl;
  if (!std::isfinite(det) || det <= 1e-9f) {
    linearization.load.conductanceSiemens = 1.0f / nominalLoad;
    return linearization;
  }
  float rhsElectrical = (le / safeDt) * speaker.electricalCurrentAmps;
  float rhsMechanical = (mass / safeDt) * speaker.coneVelocityMetersPerSecond -
                        speaker.coneDisplacementMeters / compliance;
  linearization.load.conductanceSiemens = d / det;
  linearization.load.currentAmps =
      (rhsElectrical * d - bl * rhsMechanical) / det;
  linearization.electricalCoeff = a;
  linearization.mechanicalCoeff = d;
  linearization.determinant = det;
  linearization.rhsElectrical = rhsElectrical;
  linearization.rhsMechanical = rhsMechanical;
  linearization.dt = safeDt;
  linearization.forceFactorBl = bl;
  return linearization;
}

void commitSpeakerElectricalLoad(SpeakerSim& speaker,
                                 const SpeakerElectricalLinearization& l,
                                 float appliedVolts) {
  if (!std::isfinite(l.determinant) || l.determinant <= 1e-9f) {
    return;
  }
  float drive = appliedVolts + l.rhsElectrical;
  float currentAmps =
      (drive * l.mechanicalCoeff - l.forceFactorBl * l.rhsMechanical) /
      l.determinant;
  float velocityMetersPerSecond =
      (l.electricalCoeff * l.rhsMechanical + l.forceFactorBl * drive) /
      l.determinant;
  float displacementMeters =
      speaker.coneDisplacementMeters + l.dt * velocityMetersPerSecond;
  float backEmfVolts = l.forceFactorBl * velocityMetersPerSecond;
  speaker.electricalCurrentAmps = currentAmps;
  speaker.coneVelocityMetersPerSecond = velocityMetersPerSecond;
  speaker.coneDisplacementMeters = displacementMeters;
  speaker.backEmfVolts = backEmfVolts;

  float coeff = clampf(speaker.loadSenseCoeff, 0.0f, 0.99999f);
  speaker.loadSenseVoltage =
      coeff * speaker.loadSenseVoltage + (1.0f - coeff) * std::fabs(appliedVolts);
  speaker.loadSenseCurrent =
      coeff * speaker.loadSenseCurrent + (1.0f - coeff) * std::fabs(currentAmps);
  if (speaker.loadSenseCurrent > 1e-5f && speaker.loadSenseVoltage > 1e-5f) {
    float minLoad = 0.70f * std::max(speaker.nominalLoadOhms, 1e-3f);
    float maxLoad = 4.5f * std::max(speaker.nominalLoadOhms, 1e-3f);
    speaker.effectiveLoadOhms = std::clamp(
        speaker.loadSenseVoltage / speaker.loadSenseCurrent, minLoad, maxLoad);
  }
}

OutputStageSubstepResult runOutputStageSubsteps(
    CurrentDrivenTransformer transformer,
    SpeakerSim& speaker,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float outputPrimaryLoadResistance) {
  const int transformerSubsteps = std::max(transformer.integrationSubsteps, 1);
  transformer.integrationSubsteps = 1;

  float primaryVoltageSum = 0.0f;
  float secondaryVoltageSum = 0.0f;
  float actualPlateCurrentASum = 0.0f;
  float actualPlateCurrentBSum = 0.0f;
  int totalNewtonIterations = 0;
  int maxNewtonIterations = 0;
  const power_stage_internal::TriodeLutView outputTriodeLut =
      power_stage_internal::makeTriodeLutView(power.outputTubeTriodeLut);

  for (int step = 0; step < transformerSubsteps; ++step) {
    SpeakerElectricalLinearization speakerLoad =
        linearizeSpeakerElectricalLoad(speaker, power.outputLoadResistanceOhms,
                                       transformer.dtSub);
    AffineTransformerProjection affineOut = buildAffineProjection(
        transformer, speakerLoad.load, outputPrimaryLoadResistance);
    OutputPrimarySolveResult outputSolve = solveOutputPrimaryAffineWithLut(
        affineOut, power, outputTriodeLut, outputPlateQuiescent,
        power.outputGridAVolts, power.outputGridBVolts,
        transformer.primaryVoltage);
    totalNewtonIterations += outputSolve.iterations;
    maxNewtonIterations =
        std::max(maxNewtonIterations, outputSolve.iterations);
    CurrentDrivenTransformerSample outputSample = transformer.step(
        outputSolve.driveCurrent, speakerLoad.load, outputPrimaryLoadResistance);
    float actualOutputPlateA =
        outputPlateQuiescent - 0.5f * outputSample.primaryVoltage;
    float actualOutputPlateB =
        outputPlateQuiescent + 0.5f * outputSample.primaryVoltage;
    float actualPlateCurrentA = static_cast<float>(
        power_stage_internal::evaluateTriodePlateFast(
            power.outputTubeBiasVolts + power.outputGridAVolts,
            actualOutputPlateA, power.outputTubeTriodeModel,
            power.outputTubeTriodeLut, outputTriodeLut)
            .currentAmps);
    float actualPlateCurrentB = static_cast<float>(
        power_stage_internal::evaluateTriodePlateFast(
            power.outputTubeBiasVolts + power.outputGridBVolts,
            actualOutputPlateB, power.outputTubeTriodeModel,
            power.outputTubeTriodeLut, outputTriodeLut)
            .currentAmps);
    commitSpeakerElectricalLoad(speaker, speakerLoad,
                                outputSample.secondaryVoltage);
    primaryVoltageSum += outputSample.primaryVoltage;
    secondaryVoltageSum += outputSample.secondaryVoltage;
    actualPlateCurrentASum += actualPlateCurrentA;
    actualPlateCurrentBSum += actualPlateCurrentB;
  }

  // The loop above expands the configured substeps explicitly. Preserve that
  // configuration for the next audio sample after the one-step inner solves.
  transformer.integrationSubsteps = transformerSubsteps;
  OutputStageSubstepResult result{};
  result.transformer = transformer;
  result.averagePrimaryVoltage =
      primaryVoltageSum / static_cast<float>(transformerSubsteps);
  result.averageSecondaryVoltage =
      secondaryVoltageSum / static_cast<float>(transformerSubsteps);
  result.averagePlateCurrentA =
      actualPlateCurrentASum / static_cast<float>(transformerSubsteps);
  result.averagePlateCurrentB =
      actualPlateCurrentBSum / static_cast<float>(transformerSubsteps);
  result.transformerSubsteps = transformerSubsteps;
  result.totalNewtonIterations = totalNewtonIterations;
  result.maxNewtonIterations = maxNewtonIterations;
  return result;
}

OutputStageSubstepResult runSingleEndedOutputStageSubsteps(
    CurrentDrivenTransformer transformer,
    SpeakerSim& speaker,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float outputGridVolts,
    float outputPrimaryLoadResistance) {
  const int transformerSubsteps = std::max(transformer.integrationSubsteps, 1);
  transformer.integrationSubsteps = 1;

  float primaryVoltageSum = 0.0f;
  float secondaryVoltageSum = 0.0f;
  float actualPlateCurrentSum = 0.0f;
  int totalIterations = 0;
  int maxIterations = 0;

  for (int step = 0; step < transformerSubsteps; ++step) {
    SpeakerElectricalLinearization speakerLoad =
        linearizeSpeakerElectricalLoad(speaker, power.outputLoadResistanceOhms,
                                       transformer.dtSub);
    AffineTransformerProjection affineOut = buildAffineProjection(
        transformer, speakerLoad.load, outputPrimaryLoadResistance);
    SingleEndedPrimarySolveResult outputSolve = solveSingleEndedPrimaryAffine(
        affineOut, power, outputPlateQuiescent, outputGridVolts,
        transformer.primaryVoltage);
    CurrentDrivenTransformerSample outputSample = transformer.step(
        outputSolve.driveCurrent, speakerLoad.load, outputPrimaryLoadResistance);
    commitSpeakerElectricalLoad(speaker, speakerLoad,
                                outputSample.secondaryVoltage);
    primaryVoltageSum += outputSample.primaryVoltage;
    secondaryVoltageSum += outputSample.secondaryVoltage;
    actualPlateCurrentSum += outputSolve.plateCurrent;
    totalIterations += outputSolve.iterations;
    maxIterations = std::max(maxIterations, outputSolve.iterations);
  }

  transformer.integrationSubsteps = transformerSubsteps;
  OutputStageSubstepResult result{};
  result.transformer = transformer;
  result.averagePrimaryVoltage =
      primaryVoltageSum / static_cast<float>(transformerSubsteps);
  result.averageSecondaryVoltage =
      secondaryVoltageSum / static_cast<float>(transformerSubsteps);
  result.averagePlateCurrentA =
      actualPlateCurrentSum / static_cast<float>(transformerSubsteps);
  result.averagePlateCurrentB = 0.0f;
  result.transformerSubsteps = transformerSubsteps;
  result.totalNewtonIterations = totalIterations;
  result.maxNewtonIterations = maxIterations;
  return result;
}

float estimateOutputStageNominalPowerWatts(
    const Radio1938::PowerNodeState& power) {
  float loadResistance = requirePositiveFinite(power.outputLoadResistanceOhms);
  float outputPlateQuiescent =
      requirePositiveFinite(power.outputTubeQuiescentPlateVolts);
  float maxGridPeak =
      0.92f * std::max(std::fabs(power.outputTubeBiasVolts), 1.0f);
  constexpr int kAmplitudeSteps = 40;
  constexpr int kSamplesPerCycle = 96;
  constexpr int kSettleCycles = 6;
  constexpr int kMeasureCycles = 2;
  float bestSecondaryRms = 0.0f;
  const power_stage_internal::TriodeLutView outputTriodeLut =
      power_stage_internal::makeTriodeLutView(power.outputTubeTriodeLut);

  for (int step = 1; step <= kAmplitudeSteps; ++step) {
    float gridPeak =
        maxGridPeak * static_cast<float>(step) / static_cast<float>(kAmplitudeSteps);
    CurrentDrivenTransformer transformer = power.outputTransformer;
    transformer.clearState();
    double sumSq = 0.0;
    int sampleCount = 0;

    for (int cycle = 0; cycle < (kSettleCycles + kMeasureCycles); ++cycle) {
      for (int i = 0; i < kSamplesPerCycle; ++i) {
        float phase = kRadioTwoPi * static_cast<float>(i) /
                      static_cast<float>(kSamplesPerCycle);
        float gridA = gridPeak * std::sin(phase);
        float gridB = -gridA;
        AffineTransformerProjection projection =
            buildAffineProjection(transformer, loadResistance, 0.0f);
        OutputPrimarySolveResult outputSolve = solveOutputPrimaryAffineWithLut(
            projection, power, outputTriodeLut, outputPlateQuiescent, gridA,
            gridB, transformer.primaryVoltage);
        auto outputSample =
            transformer.step(outputSolve.driveCurrent, loadResistance, 0.0f);
        if (cycle >= kSettleCycles) {
          sumSq += static_cast<double>(outputSample.secondaryVoltage) *
                   static_cast<double>(outputSample.secondaryVoltage);
          sampleCount++;
        }
      }
    }

    float secondaryRms =
        static_cast<float>(std::sqrt(sumSq / std::max(sampleCount, 1)));
    bestSecondaryRms = std::max(bestSecondaryRms, secondaryRms);
  }

  return (bestSecondaryRms * bestSecondaryRms) / loadResistance;
}

float estimateSingleEndedOutputStageNominalPowerWatts(
    const Radio1938::PowerNodeState& power) {
  float loadResistance = requirePositiveFinite(power.outputLoadResistanceOhms);
  float outputPlateQuiescent =
      requirePositiveFinite(power.outputTubeQuiescentPlateVolts);
  float maxGridPeak =
      0.92f * std::max(std::fabs(power.outputTubeBiasVolts), 1.0f);
  constexpr int kAmplitudeSteps = 40;
  constexpr int kSamplesPerCycle = 96;
  constexpr int kSettleCycles = 6;
  constexpr int kMeasureCycles = 2;
  float bestSecondaryRms = 0.0f;

  for (int step = 1; step <= kAmplitudeSteps; ++step) {
    float gridPeak =
        maxGridPeak * static_cast<float>(step) / static_cast<float>(kAmplitudeSteps);
    CurrentDrivenTransformer transformer = power.outputTransformer;
    transformer.clearState();
    double sumSq = 0.0;
    int sampleCount = 0;

    for (int cycle = 0; cycle < (kSettleCycles + kMeasureCycles); ++cycle) {
      for (int i = 0; i < kSamplesPerCycle; ++i) {
        float phase = kRadioTwoPi * static_cast<float>(i) /
                      static_cast<float>(kSamplesPerCycle);
        float grid = gridPeak * std::sin(phase);
        AffineTransformerProjection projection =
            buildAffineProjection(transformer, loadResistance, 0.0f);
        SingleEndedPrimarySolveResult outputSolve =
            solveSingleEndedPrimaryAffine(projection, power,
                                           outputPlateQuiescent, grid,
                                           transformer.primaryVoltage);
        CurrentDrivenTransformerSample outputSample = transformer.step(
            outputSolve.driveCurrent, loadResistance, 0.0f);
        if (cycle >= kSettleCycles) {
          sumSq += static_cast<double>(outputSample.secondaryVoltage) *
                   static_cast<double>(outputSample.secondaryVoltage);
          sampleCount++;
        }
      }
    }

    float secondaryRms =
        static_cast<float>(std::sqrt(sumSq / std::max(sampleCount, 1)));
    bestSecondaryRms = std::max(bestSecondaryRms, secondaryRms);
  }

  return (bestSecondaryRms * bestSecondaryRms) / loadResistance;
}
