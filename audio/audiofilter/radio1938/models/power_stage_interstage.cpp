#include "power_stage_internal.h"

#include "../../math/signal_math.h"
#include "../../math/linear_solvers.h"
#include "../../models/tube_models.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace power_stage_internal {

int chooseAdaptiveInterstageSubsteps(
    const CurrentDrivenTransformer& transformer,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts) {
  int maxSubsteps = std::max(transformer.integrationSubsteps, 1);
  if (maxSubsteps <= 2) {
    return maxSubsteps;
  }

  int mediumSubsteps = std::max(2, (maxSubsteps + 1) / 2);
  int quietSubsteps = std::max(2, (maxSubsteps + 3) / 4);
  float driverBiasMagnitude = std::max(std::fabs(power.tubeBiasVolts), 1.0f);
  float outputBiasMagnitude =
      std::max(std::fabs(power.outputTubeBiasVolts), 1.0f);
  float driverSignalRatio =
      std::fabs(controlGridVolts - power.tubeBiasVolts) / driverBiasMagnitude;
  float outputSignalRatio =
      std::max(std::fabs(power.interstageCt.secondaryAVoltage),
               std::fabs(power.interstageCt.secondaryBVoltage)) /
      outputBiasMagnitude;
  float primarySwingRatio =
      std::fabs(power.interstageCt.primaryVoltage) /
      std::max(power.tubeQuiescentPlateVolts, 1.0f);
  float outputGridTotalA =
      power.outputTubeBiasVolts + power.interstageCt.secondaryAVoltage;
  float outputGridTotalB =
      power.outputTubeBiasVolts + power.interstageCt.secondaryBVoltage;
  float conductionMargin = std::max(outputGridTotalA, outputGridTotalB);

  if (conductionMargin > -2.0f || driverSignalRatio > 0.30f ||
      outputSignalRatio > 0.30f || primarySwingRatio > 0.10f) {
    return maxSubsteps;
  }
  if (conductionMargin > -8.0f || driverSignalRatio > 0.08f ||
      outputSignalRatio > 0.08f || primarySwingRatio > 0.03f) {
    return mediumSubsteps;
  }
  return quietSubsteps;
}

int nextLowerAdaptiveSubsteps(int currentSubsteps) {
  if (currentSubsteps <= 1) {
    return 1;
  }
  if (currentSubsteps <= 2) {
    return 1;
  }
  return std::max(2, currentSubsteps / 2);
}

float estimateInterstageBoundaryErrorVolts(
    const DriverInterstageCenterTappedResult& coarse,
    const DriverInterstageCenterTappedResult& fine) {
  float primaryError = std::fabs(coarse.primaryVoltage - fine.primaryVoltage);
  float secondaryAError =
      std::fabs(coarse.secondaryAVoltage - fine.secondaryAVoltage);
  float secondaryBError =
      std::fabs(coarse.secondaryBVoltage - fine.secondaryBVoltage);
  return std::max(primaryError,
                  std::max(secondaryAError, secondaryBError));
}

namespace {

DriverInterstageCenterTappedResult solveDriverInterstageCenterTappedNoCapAtSubsteps(
    const CurrentDrivenTransformer& transformer,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts,
    float driverPlateQuiescent,
    float driverQuiescentCurrent,
    int substeps,
    bool measureDriverEvalTime) {
  assert(power.tubeTriodeConnected);

  int configuredSubsteps = std::max(transformer.integrationSubsteps, 1);
  int clampedSubsteps = std::clamp(substeps, 1, configuredSubsteps);
  float sampleDt =
      requirePositiveFinite(transformer.dtSub) *
      static_cast<float>(configuredSubsteps);
  float dt = sampleDt / static_cast<float>(clampedSubsteps);
  float turns = requirePositiveFinite(transformer.cachedTurns);
  float halfTurns = 2.0f * turns;

  float rp = transformer.primaryResistanceOhms;
  float ra = 0.5f * transformer.secondaryResistanceOhms;
  float rb = 0.5f * transformer.secondaryResistanceOhms;

  float lp = transformer.cachedPrimaryInductance;
  float lLeakHalf = 0.5f * transformer.secondaryLeakageInductanceHenries;
  float lHalfMag = transformer.magnetizingInductanceHenries /
                   (halfTurns * halfTurns);
  float la = lLeakHalf + lHalfMag;
  float lb = lLeakHalf + lHalfMag;
  float mab = lHalfMag;
  float mutual = 0.5f * transformer.cachedMutualInductance;

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
  float lpOverDt = lp / dt;
  float laOverDt = la / dt;
  float lbOverDt = lb / dt;
  float mOverDt = mutual / dt;
  float mabOverDt = mab / dt;
  float primarySeries = rp + lpOverDt;
  float secondaryASeries = ra + laOverDt;
  float secondaryBSeries = rb + lbOverDt;
  float gridLeakConductance =
      1.0f / requirePositiveFinite(power.outputGridLeakResistanceOhms);
  float gridCurrentConductance =
      1.0f / requirePositiveFinite(power.outputGridCurrentResistanceOhms);
  TriodeLutView driverTriodeLut = makeTriodeLutView(power.tubeTriodeLut);
  constexpr float kInterstageConvergenceTolerance = 5e-5f;
  constexpr uint64_t kDriverEvalTimingStride = 32;
  int totalIterations = 0;
  int maxIterations = 0;
  uint64_t driverEvalCount = 0;
  uint64_t driverEvalTimeNs = 0;

  for (int step = 0; step < clampedSubsteps; ++step) {
    float ipPrev = primaryCurrent;
    float iaPrev = secondaryACurrent;
    float ibPrev = secondaryBCurrent;
    float cPrimary = lpOverDt * ipPrev + mOverDt * iaPrev - mOverDt * ibPrev;
    float cSecondaryA = mOverDt * ipPrev + laOverDt * iaPrev - mabOverDt * ibPrev;
    float cSecondaryB =
        -mOverDt * ipPrev - mabOverDt * iaPrev + lbOverDt * ibPrev;

    for (int iter = 0; iter < 12; ++iter) {
      totalIterations++;
      maxIterations = std::max(maxIterations, iter + 1);
      float driverPlateVolts = driverPlateQuiescent - primaryVoltage;
      driverEvalCount++;
      bool sampleDriverEvalTime =
          measureDriverEvalTime &&
          ((driverEvalCount - 1u) % kDriverEvalTimingStride == 0u);
      uint64_t driverEvalStartNs =
          sampleDriverEvalTime ? monotonicNowNs() : 0;
      KorenTriodePlateEval driverEval = evaluateTriodePlateFast(
          controlGridVolts, driverPlateVolts, power.tubeTriodeModel,
          power.tubeTriodeLut, driverTriodeLut);
      if (sampleDriverEvalTime) {
        driverEvalTimeNs +=
            (monotonicNowNs() - driverEvalStartNs) * kDriverEvalTimingStride;
      }
      float driverPlateCurrentAbs = static_cast<float>(driverEval.currentAmps);
      float dIdriveDvp = -static_cast<float>(driverEval.conductanceSiemens);
      float driveCurrent = driverPlateCurrentAbs - driverQuiescentCurrent;
      float primaryCurrentNow = driveCurrent - coreLossConductance * primaryVoltage;

      TubeGridBranchEval aBranch = evaluateTubeGridBranch(
          secondaryAVoltage, power.outputTubeBiasVolts, gridLeakConductance,
          gridCurrentConductance);
      float secondaryACurrentNow = -aBranch.current;
      float dIaDva = -aBranch.slope;
      TubeGridBranchEval bBranch = evaluateTubeGridBranch(
          secondaryBVoltage, power.outputTubeBiasVolts, gridLeakConductance,
          gridCurrentConductance);
      float secondaryBCurrentNow = -bBranch.current;
      float dIbDvb = -bBranch.slope;
      float dIpDvp = dIdriveDvp - coreLossConductance;

      float f0 = primarySeries * primaryCurrentNow +
                 mOverDt * secondaryACurrentNow -
                 mOverDt * secondaryBCurrentNow - primaryVoltage - cPrimary;
      float f1 = mOverDt * primaryCurrentNow +
                 secondaryASeries * secondaryACurrentNow -
                 mabOverDt * secondaryBCurrentNow - secondaryAVoltage -
                 cSecondaryA;
      float f2 = -mOverDt * primaryCurrentNow -
                 mabOverDt * secondaryACurrentNow +
                 secondaryBSeries * secondaryBCurrentNow - secondaryBVoltage -
                 cSecondaryB;

      float a00 = primarySeries * dIpDvp - 1.0f;
      float a01 = mOverDt * dIaDva;
      float a02 = -mOverDt * dIbDvb;
      float a10 = mOverDt * dIpDvp;
      float a11 = secondaryASeries * dIaDva - 1.0f;
      float a12 = -mabOverDt * dIbDvb;
      float a20 = -mOverDt * dIpDvp;
      float a21 = -mabOverDt * dIaDva;
      float a22 = secondaryBSeries * dIbDvb - 1.0f;

      float rhs[3] = {-f0, -f1, -f2};
      float delta[3] = {};
      bool solved = solveLinear3x3Direct(a00, a01, a02, a10, a11, a12, a20,
                                         a21, a22, rhs, delta);
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
      if (maxDelta < kInterstageConvergenceTolerance) break;
    }

    float driverPlateVolts = driverPlateQuiescent - primaryVoltage;
    driverEvalCount++;
    bool sampleDriverEvalTime =
        measureDriverEvalTime &&
        ((driverEvalCount - 1u) % kDriverEvalTimingStride == 0u);
    uint64_t driverEvalStartNs =
        sampleDriverEvalTime ? monotonicNowNs() : 0;
    float driverPlateCurrentAbs =
        static_cast<float>(evaluateTriodePlateFast(
                               controlGridVolts, driverPlateVolts,
                               power.tubeTriodeModel, power.tubeTriodeLut,
                               driverTriodeLut)
                               .currentAmps);
    if (sampleDriverEvalTime) {
      driverEvalTimeNs +=
          (monotonicNowNs() - driverEvalStartNs) * kDriverEvalTimingStride;
    }
    primaryCurrent =
        driverPlateCurrentAbs - driverQuiescentCurrent -
        coreLossConductance * primaryVoltage;
    secondaryACurrent = -evaluateTubeGridBranch(
                             secondaryAVoltage, power.outputTubeBiasVolts,
                             gridLeakConductance, gridCurrentConductance)
                             .current;
    secondaryBCurrent = -evaluateTubeGridBranch(
                             secondaryBVoltage, power.outputTubeBiasVolts,
                             gridLeakConductance, gridCurrentConductance)
                             .current;
  }

  float finalDriverPlateVolts = driverPlateQuiescent - primaryVoltage;
  driverEvalCount++;
  bool sampleFinalDriverEvalTime =
      measureDriverEvalTime &&
      ((driverEvalCount - 1u) % kDriverEvalTimingStride == 0u);
  uint64_t finalDriverEvalStartNs =
      sampleFinalDriverEvalTime ? monotonicNowNs() : 0;
  float finalDriverPlateCurrentAbs =
      static_cast<float>(evaluateTriodePlateFast(
                             controlGridVolts, finalDriverPlateVolts,
                             power.tubeTriodeModel, power.tubeTriodeLut,
                             driverTriodeLut)
                             .currentAmps);
  if (sampleFinalDriverEvalTime) {
    driverEvalTimeNs +=
        (monotonicNowNs() - finalDriverEvalStartNs) * kDriverEvalTimingStride;
  }

  DriverInterstageCenterTappedResult result{};
  result.driverPlateCurrentAbs = finalDriverPlateCurrentAbs;
  result.primaryCurrent = primaryCurrent;
  result.primaryVoltage = primaryVoltage;
  result.secondaryACurrent = secondaryACurrent;
  result.secondaryAVoltage = secondaryAVoltage;
  result.secondaryBCurrent = secondaryBCurrent;
  result.secondaryBVoltage = secondaryBVoltage;
  result.substeps = clampedSubsteps;
  result.totalIterations = totalIterations;
  result.maxIterations = maxIterations;
  result.driverEvalCount = driverEvalCount;
  result.driverEvalTimeNs = driverEvalTimeNs;
  return result;
}

}  // namespace

}  // namespace power_stage_internal

DriverInterstageCenterTappedResult solveDriverInterstageCenterTappedNoCap(
    const CurrentDrivenTransformer& transformer,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts,
    float driverPlateQuiescent,
    float driverQuiescentCurrent,
    bool measureDriverEvalTime) {
  constexpr int kAdaptiveValidationCooldown = 8;
  constexpr int kAdaptiveExtendedValidationCooldown = 16;
  constexpr int kAdaptiveDeepValidationCooldown = 24;
  constexpr float kAdaptiveStepErrorToleranceVolts = 0.25f;

  int configuredSubsteps = std::max(transformer.integrationSubsteps, 1);
  int heuristicSubsteps = power_stage_internal::chooseAdaptiveInterstageSubsteps(
      transformer, power, controlGridVolts);
  int currentAdaptiveSubsteps =
      (power.interstageAdaptiveSubsteps > 0) ? power.interstageAdaptiveSubsteps
                                             : configuredSubsteps;
  currentAdaptiveSubsteps =
      std::clamp(currentAdaptiveSubsteps, 1, configuredSubsteps);
  int currentValidationCountdown =
      std::max(power.interstageAdaptiveValidationCountdown, 0);

  if (heuristicSubsteps > currentAdaptiveSubsteps) {
    DriverInterstageCenterTappedResult raised =
        power_stage_internal::solveDriverInterstageCenterTappedNoCapAtSubsteps(
            transformer, power, controlGridVolts, driverPlateQuiescent,
            driverQuiescentCurrent, heuristicSubsteps, measureDriverEvalTime);
    raised.suggestedSubsteps = heuristicSubsteps;
    raised.suggestedValidationCountdown = kAdaptiveValidationCooldown;
    return raised;
  }

  if (heuristicSubsteps < currentAdaptiveSubsteps &&
      currentValidationCountdown <= 0) {
    int trialSubsteps = std::max(
        heuristicSubsteps,
        power_stage_internal::nextLowerAdaptiveSubsteps(currentAdaptiveSubsteps));
    trialSubsteps = std::clamp(trialSubsteps, 1, currentAdaptiveSubsteps);
    DriverInterstageCenterTappedResult fine =
        power_stage_internal::solveDriverInterstageCenterTappedNoCapAtSubsteps(
            transformer, power, controlGridVolts, driverPlateQuiescent,
            driverQuiescentCurrent, currentAdaptiveSubsteps,
            measureDriverEvalTime);
    DriverInterstageCenterTappedResult coarse =
        power_stage_internal::solveDriverInterstageCenterTappedNoCapAtSubsteps(
            transformer, power, controlGridVolts, driverPlateQuiescent,
            driverQuiescentCurrent, trialSubsteps, measureDriverEvalTime);
    float errorVolts = power_stage_internal::estimateInterstageBoundaryErrorVolts(
        coarse, fine);
    coarse.adaptiveValidationAttempts = 1;
    coarse.adaptiveBoundaryErrorVolts = errorVolts;
    fine.adaptiveValidationAttempts = 1;
    fine.adaptiveBoundaryErrorVolts = errorVolts;
    if (errorVolts <= kAdaptiveStepErrorToleranceVolts) {
      int acceptedCooldown = kAdaptiveValidationCooldown;
      if (trialSubsteps > heuristicSubsteps) {
        float errorRatio =
            errorVolts / std::max(kAdaptiveStepErrorToleranceVolts, 1e-6f);
        if (errorRatio <= 0.35f) {
          acceptedCooldown = kAdaptiveDeepValidationCooldown;
        } else if (errorRatio <= 0.65f) {
          acceptedCooldown = kAdaptiveExtendedValidationCooldown;
        }
      }
      coarse.adaptiveAcceptedDownshifts = 1;
      coarse.suggestedSubsteps = trialSubsteps;
      coarse.suggestedValidationCountdown = acceptedCooldown;
      return coarse;
    }
    fine.suggestedSubsteps = currentAdaptiveSubsteps;
    fine.suggestedValidationCountdown = kAdaptiveValidationCooldown;
    return fine;
  }

  DriverInterstageCenterTappedResult steady =
      power_stage_internal::solveDriverInterstageCenterTappedNoCapAtSubsteps(
          transformer, power, controlGridVolts, driverPlateQuiescent,
          driverQuiescentCurrent, currentAdaptiveSubsteps,
          measureDriverEvalTime);
  steady.suggestedSubsteps = currentAdaptiveSubsteps;
  steady.suggestedValidationCountdown =
      std::max(currentValidationCountdown - 1, 0);
  return steady;
}
