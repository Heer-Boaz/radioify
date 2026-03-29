#include "power_stage_solver.h"

#include "../../math/linear_solvers.h"
#include "../../math/signal_math.h"
#include "../../models/tube_models.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace {

struct OutputTubePairEval {
  float plateCurrentA = 0.0f;
  float plateCurrentB = 0.0f;
  float driveCurrent = 0.0f;
  float driveSlope = 0.0f;
};

float tubeGridBranchCurrent(float acGridVolts,
                            float biasVolts,
                            float gridLeakResistanceOhms,
                            float gridCurrentResistanceOhms) {
  float leakCurrent = acGridVolts / gridLeakResistanceOhms;
  float positiveGridCurrent =
      (biasVolts + acGridVolts > 0.0f)
          ? ((biasVolts + acGridVolts) / gridCurrentResistanceOhms)
          : 0.0f;
  return leakCurrent + positiveGridCurrent;
}

float tubeGridBranchSlope(float acGridVolts,
                          float biasVolts,
                          float gridLeakResistanceOhms,
                          float gridCurrentResistanceOhms) {
  float slope = 1.0f / gridLeakResistanceOhms;
  if (biasVolts + acGridVolts > 0.0f) {
    slope += 1.0f / gridCurrentResistanceOhms;
  }
  return slope;
}

OutputTubePairEval evaluateOutputTubePair(
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float gridA,
    float gridB,
    float primaryVoltage) {
  float plateA = outputPlateQuiescent - 0.5f * primaryVoltage;
  float plateB = outputPlateQuiescent + 0.5f * primaryVoltage;
  KorenTriodePlateEval evalA = evaluateKorenTriodePlateRuntime(
      power.outputTubeBiasVolts + gridA, plateA, power.outputTubeTriodeModel,
      power.outputTubeTriodeLut);
  KorenTriodePlateEval evalB = evaluateKorenTriodePlateRuntime(
      power.outputTubeBiasVolts + gridB, plateB, power.outputTubeTriodeModel,
      power.outputTubeTriodeLut);

  OutputTubePairEval result{};
  result.plateCurrentA = static_cast<float>(evalA.currentAmps);
  result.plateCurrentB = static_cast<float>(evalB.currentAmps);
  result.driveCurrent = 0.5f * (result.plateCurrentA - result.plateCurrentB);
  result.driveSlope = -0.25f * static_cast<float>(evalA.conductanceSiemens +
                                                  evalB.conductanceSiemens);
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
  float primaryVoltage = initialPrimaryVoltage;
  constexpr float kPrimaryVoltageTolerance = 1e-5f;

  for (int iter = 0; iter < 8; ++iter) {
    OutputTubePairEval pair = evaluateOutputTubePair(
        power, outputPlateQuiescent, gridA, gridB, primaryVoltage);

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

  OutputTubePairEval finalPair = evaluateOutputTubePair(
      power, outputPlateQuiescent, gridA, gridB, primaryVoltage);
  OutputPrimarySolveResult result{};
  result.primaryVoltage = primaryVoltage;
  result.driveCurrent = finalPair.driveCurrent;
  result.plateCurrentA = finalPair.plateCurrentA;
  result.plateCurrentB = finalPair.plateCurrentB;
  return result;
}

SpeakerElectricalLinearization linearizeSpeakerElectricalLoad(
    const SpeakerSim& speaker,
    float nominalLoadOhms,
    float dt) {
  SpeakerElectricalLinearization linearization{};
  float nominalLoad = requirePositiveFinite(nominalLoadOhms);
  float re = std::max(speaker.voiceCoilResistanceOhms, 1e-4f);
  float le = std::max(speaker.voiceCoilInductanceHenries, 1e-7f);
  float mass = std::max(speaker.movingMassKg, 1e-6f);
  float compliance =
      std::max(speaker.suspensionComplianceMetersPerNewton, 1e-8f);
  float damping = std::max(speaker.mechanicalDampingNsPerMeter, 1e-5f);
  float bl = std::max(speaker.forceFactorBl, 1e-4f);
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

  for (int step = 0; step < transformerSubsteps; ++step) {
    SpeakerElectricalLinearization speakerLoad =
        linearizeSpeakerElectricalLoad(speaker, power.outputLoadResistanceOhms,
                                       transformer.dtSub);
    AffineTransformerProjection affineOut = buildAffineProjection(
        transformer, speakerLoad.load, outputPrimaryLoadResistance);
    OutputPrimarySolveResult outputSolve = solveOutputPrimaryAffine(
        affineOut, power, outputPlateQuiescent, power.outputGridAVolts,
        power.outputGridBVolts, transformer.primaryVoltage);
    CurrentDrivenTransformerSample outputSample = transformer.step(
        outputSolve.driveCurrent, speakerLoad.load, outputPrimaryLoadResistance);
    float actualOutputPlateA =
        outputPlateQuiescent - 0.5f * outputSample.primaryVoltage;
    float actualOutputPlateB =
        outputPlateQuiescent + 0.5f * outputSample.primaryVoltage;
    float actualPlateCurrentA = static_cast<float>(
        evaluateKorenTriodePlateRuntime(
            power.outputTubeBiasVolts + power.outputGridAVolts,
            actualOutputPlateA, power.outputTubeTriodeModel,
            power.outputTubeTriodeLut)
            .currentAmps);
    float actualPlateCurrentB = static_cast<float>(
        evaluateKorenTriodePlateRuntime(
            power.outputTubeBiasVolts + power.outputGridBVolts,
            actualOutputPlateB, power.outputTubeTriodeModel,
            power.outputTubeTriodeLut)
            .currentAmps);
    commitSpeakerElectricalLoad(speaker, speakerLoad,
                                outputSample.secondaryVoltage);
    primaryVoltageSum += outputSample.primaryVoltage;
    secondaryVoltageSum += outputSample.secondaryVoltage;
    actualPlateCurrentASum += actualPlateCurrentA;
    actualPlateCurrentBSum += actualPlateCurrentB;
  }

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
        OutputPrimarySolveResult outputSolve = solveOutputPrimaryAffine(
            projection, power, outputPlateQuiescent, gridA, gridB,
            transformer.primaryVoltage);
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

DriverInterstageCenterTappedResult solveDriverInterstageCenterTappedNoCap(
    const CurrentDrivenTransformer& transformer,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts,
    float driverPlateQuiescent,
    float driverQuiescentCurrent) {
  assert(power.tubeTriodeConnected);

  float dt = requirePositiveFinite(transformer.dtSub);
  float turns = requirePositiveFinite(transformer.cachedTurns);
  float halfTurns = 2.0f * turns;

  float Rp = transformer.primaryResistanceOhms;
  float Ra = 0.5f * transformer.secondaryResistanceOhms;
  float Rb = 0.5f * transformer.secondaryResistanceOhms;

  float Lp = transformer.cachedPrimaryInductance;
  float LlkHalf = 0.5f * transformer.secondaryLeakageInductanceHenries;
  float LhalfMag = transformer.magnetizingInductanceHenries /
                   (halfTurns * halfTurns);
  float La = LlkHalf + LhalfMag;
  float Lb = LlkHalf + LhalfMag;
  float Mab = LhalfMag;
  float M = 0.5f * transformer.cachedMutualInductance;

  float coreLossConductance =
      (transformer.primaryCoreLossResistanceOhms > 0.0f)
          ? (1.0f / transformer.primaryCoreLossResistanceOhms)
          : 0.0f;

  float primaryVoltage = power.interstageCt.primaryVoltage;
  float secondaryAVoltage = power.interstageCt.secondaryAVoltage;
  float secondaryBVoltage = power.interstageCt.secondaryBVoltage;
  float primaryCurrent = power.interstageCt.primaryCurrent;
  float secondaryACurrent = power.interstageCt.secondaryACurrent;
  float secondaryBCurrent = power.interstageCt.secondaryBCurrent;
  float lpOverDt = Lp / dt;
  float laOverDt = La / dt;
  float lbOverDt = Lb / dt;
  float mOverDt = M / dt;
  float mabOverDt = Mab / dt;

  for (int step = 0; step < transformer.integrationSubsteps; ++step) {
    float ipPrev = primaryCurrent;
    float iaPrev = secondaryACurrent;
    float ibPrev = secondaryBCurrent;
    float cPrimary = lpOverDt * ipPrev + mOverDt * iaPrev - mOverDt * ibPrev;
    float cSecondaryA = mOverDt * ipPrev + laOverDt * iaPrev - mabOverDt * ibPrev;
    float cSecondaryB =
        -mOverDt * ipPrev - mabOverDt * iaPrev + lbOverDt * ibPrev;

    for (int iter = 0; iter < 12; ++iter) {
      float driverPlateVolts = driverPlateQuiescent - primaryVoltage;
      KorenTriodePlateEval driverEval = evaluateKorenTriodePlateRuntime(
          controlGridVolts, driverPlateVolts, power.tubeTriodeModel,
          power.tubeTriodeLut);
      float driverPlateCurrentAbs = static_cast<float>(driverEval.currentAmps);
      float dIdriveDvp = -static_cast<float>(driverEval.conductanceSiemens);
      float driveCurrent = driverPlateCurrentAbs - driverQuiescentCurrent;
      float primaryCurrentNow = driveCurrent - coreLossConductance * primaryVoltage;

      float iaBranch = tubeGridBranchCurrent(
          secondaryAVoltage, power.outputTubeBiasVolts,
          power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float secondaryACurrentNow = -iaBranch;
      float dIaDva = -tubeGridBranchSlope(
          secondaryAVoltage, power.outputTubeBiasVolts,
          power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float ibBranch = tubeGridBranchCurrent(
          secondaryBVoltage, power.outputTubeBiasVolts,
          power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float secondaryBCurrentNow = -ibBranch;
      float dIbDvb = -tubeGridBranchSlope(
          secondaryBVoltage, power.outputTubeBiasVolts,
          power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float dIpDvp = dIdriveDvp - coreLossConductance;

      float f[3] = {
          (Rp + lpOverDt) * primaryCurrentNow +
                  mOverDt * secondaryACurrentNow -
                  mOverDt * secondaryBCurrentNow -
                  primaryVoltage -
                  cPrimary,
          mOverDt * primaryCurrentNow +
                  (Ra + laOverDt) * secondaryACurrentNow -
                  mabOverDt * secondaryBCurrentNow -
                  secondaryAVoltage -
                  cSecondaryA,
          -mOverDt * primaryCurrentNow -
                  mabOverDt * secondaryACurrentNow +
                  (Rb + lbOverDt) * secondaryBCurrentNow -
                  secondaryBVoltage -
                  cSecondaryB,
      };

      float j[3][3] = {
          {(Rp + lpOverDt) * dIpDvp - 1.0f, mOverDt * dIaDva,
           -mOverDt * dIbDvb},
          {mOverDt * dIpDvp, (Ra + laOverDt) * dIaDva - 1.0f,
           -mabOverDt * dIbDvb},
          {-mOverDt * dIpDvp, -mabOverDt * dIaDva,
           (Rb + lbOverDt) * dIbDvb - 1.0f},
      };

      float rhs[3] = {-f[0], -f[1], -f[2]};
      float delta[3] = {};
      bool solved = solveLinear3x3Direct(j[0][0], j[0][1], j[0][2], j[1][0],
                                         j[1][1], j[1][2], j[2][0], j[2][1],
                                         j[2][2], rhs, delta);
      assert(solved && "interstage 3x3 solve failed");
      (void)solved;

      primaryVoltage += delta[0];
      secondaryAVoltage += delta[1];
      secondaryBVoltage += delta[2];

      float maxDelta = std::max(std::fabs(delta[0]),
                                std::max(std::fabs(delta[1]),
                                         std::fabs(delta[2])));
      assert(std::isfinite(primaryVoltage));
      assert(std::isfinite(secondaryAVoltage));
      assert(std::isfinite(secondaryBVoltage));
      if (maxDelta < 1e-6f) break;
    }

    float driverPlateVolts = driverPlateQuiescent - primaryVoltage;
    float driverPlateCurrentAbs = static_cast<float>(
        evaluateKorenTriodePlateRuntime(controlGridVolts, driverPlateVolts,
                                        power.tubeTriodeModel,
                                        power.tubeTriodeLut)
            .currentAmps);
    primaryCurrent =
        driverPlateCurrentAbs - driverQuiescentCurrent -
        coreLossConductance * primaryVoltage;
    secondaryACurrent = -tubeGridBranchCurrent(
        secondaryAVoltage, power.outputTubeBiasVolts,
        power.outputGridLeakResistanceOhms,
        power.outputGridCurrentResistanceOhms);
    secondaryBCurrent = -tubeGridBranchCurrent(
        secondaryBVoltage, power.outputTubeBiasVolts,
        power.outputGridLeakResistanceOhms,
        power.outputGridCurrentResistanceOhms);
  }

  float finalDriverPlateVolts = driverPlateQuiescent - primaryVoltage;
  float finalDriverPlateCurrentAbs = static_cast<float>(
      evaluateKorenTriodePlateRuntime(controlGridVolts, finalDriverPlateVolts,
                                      power.tubeTriodeModel,
                                      power.tubeTriodeLut)
          .currentAmps);

  DriverInterstageCenterTappedResult result{};
  result.driverPlateCurrentAbs = finalDriverPlateCurrentAbs;
  result.primaryCurrent = primaryCurrent;
  result.primaryVoltage = primaryVoltage;
  result.secondaryACurrent = secondaryACurrent;
  result.secondaryAVoltage = secondaryAVoltage;
  result.secondaryBCurrent = secondaryBCurrent;
  result.secondaryBVoltage = secondaryBVoltage;
  return result;
}
