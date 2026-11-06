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
    const CurrentDrivenTransformer& transformer, const Radio1938::PowerNodeState& power,
    float controlGridVolts, float driverPlateQuiescent, float driverQuiescentCurrent,
    int substeps) {
  assert(power.tubeTriodeConnected);

  int configuredSubsteps = std::max(transformer.integrationSubsteps, 1);
  int clampedSubsteps = std::clamp(substeps, 1, configuredSubsteps);
  double sampleDt =
      requirePositiveFinite(transformer.dtSub) * static_cast<double>(configuredSubsteps);
  double dt = sampleDt / static_cast<double>(clampedSubsteps);
  double turns = requirePositiveFinite(transformer.cachedTurns);
  double halfTurns = 2.0f * turns;

  double rp = transformer.primaryResistanceOhms;
  double ra = 0.5f * transformer.secondaryResistanceOhms;
  double rb = 0.5f * transformer.secondaryResistanceOhms;

  double lp = transformer.cachedPrimaryInductance;
  double lLeakHalf = 0.5f * transformer.secondaryLeakageInductanceHenries;
  double lHalfMag = transformer.magnetizingInductanceHenries / (halfTurns * halfTurns);
  double la = lLeakHalf + lHalfMag;
  double lb = lLeakHalf + lHalfMag;
  double mab = lHalfMag;
  double mutual = 0.5f * transformer.cachedMutualInductance;

  double coreLossConductance = (transformer.primaryCoreLossResistanceOhms > 0.0f)
                                   ? (1.0f / transformer.primaryCoreLossResistanceOhms)
                                   : 0.0f;

  double primaryVoltage = power.interstageCt.primaryVoltage;
  double secondaryAVoltage = power.interstageCt.secondaryAVoltage;
  double secondaryBVoltage = power.interstageCt.secondaryBVoltage;
  double primaryCurrent = power.interstageCt.primaryCurrent;
  double secondaryACurrent = power.interstageCt.secondaryACurrent;
  double secondaryBCurrent = power.interstageCt.secondaryBCurrent;
  double lpOverDt = lp / dt;
  double laOverDt = la / dt;
  double lbOverDt = lb / dt;
  double mOverDt = mutual / dt;
  double mabOverDt = mab / dt;
  double primarySeries = rp + lpOverDt;
  double secondaryASeries = ra + laOverDt;
  double secondaryBSeries = rb + lbOverDt;
  double gridLeakConductance =
      1.0f / requirePositiveFinite(power.outputGridLeakResistanceOhms);
  double gridCurrentConductance =
      1.0f / requirePositiveFinite(power.outputGridCurrentResistanceOhms);
  TriodeLutView driverTriodeLut = makeTriodeLutView(power.tubeTriodeLut);
  constexpr double kInterstageConvergenceTolerance = 5e-5f;
  // Solve the coupled residual in double precision. Float subtraction of the
  // large inductive history terms can exceed the 50 microvolt tolerance and
  // keep an already settled substep iterating until its iteration limit.
  double driverPlateCurrentAbs = 0.0;
  int totalIterations = 0;
  int maxIterations = 0;
  uint64_t driverEvalCount = 0;

  for (int step = 0; step < clampedSubsteps; ++step) {
    double ipPrev = primaryCurrent;
    double iaPrev = secondaryACurrent;
    double ibPrev = secondaryBCurrent;
    double cPrimary = lpOverDt * ipPrev + mOverDt * iaPrev - mOverDt * ibPrev;
    double cSecondaryA = mOverDt * ipPrev + laOverDt * iaPrev - mabOverDt * ibPrev;
    double cSecondaryB = -mOverDt * ipPrev - mabOverDt * iaPrev + lbOverDt * ibPrev;

    for (int iter = 0; iter < 12; ++iter) {
      totalIterations++;
      maxIterations = std::max(maxIterations, iter + 1);
      double driverPlateVolts = driverPlateQuiescent - primaryVoltage;
      driverEvalCount++;
      KorenTriodePlateEval driverEval = evaluateTriodePlateFast(
          controlGridVolts, driverPlateVolts, power.tubeTriodeModel, power.tubeTriodeLut,
          driverTriodeLut);
      driverPlateCurrentAbs = driverEval.currentAmps;
      double dIdriveDvp = -driverEval.conductanceSiemens;
      double driveCurrent = driverPlateCurrentAbs - driverQuiescentCurrent;
      double primaryCurrentNow = driveCurrent - coreLossConductance * primaryVoltage;

      TubeGridBranchEval aBranch =
          evaluateTubeGridBranch(secondaryAVoltage, power.outputTubeBiasVolts,
                                 gridLeakConductance, gridCurrentConductance);
      double secondaryACurrentNow = -aBranch.current;
      double dIaDva = -aBranch.slope;
      TubeGridBranchEval bBranch =
          evaluateTubeGridBranch(secondaryBVoltage, power.outputTubeBiasVolts,
                                 gridLeakConductance, gridCurrentConductance);
      double secondaryBCurrentNow = -bBranch.current;
      double dIbDvb = -bBranch.slope;
      double dIpDvp = dIdriveDvp - coreLossConductance;

      double f0 = primarySeries * primaryCurrentNow + mOverDt * secondaryACurrentNow -
                  mOverDt * secondaryBCurrentNow - primaryVoltage - cPrimary;
      double f1 = mOverDt * primaryCurrentNow + secondaryASeries * secondaryACurrentNow -
                  mabOverDt * secondaryBCurrentNow - secondaryAVoltage - cSecondaryA;
      double f2 = -mOverDt * primaryCurrentNow - mabOverDt * secondaryACurrentNow +
                  secondaryBSeries * secondaryBCurrentNow - secondaryBVoltage -
                  cSecondaryB;

      double a00 = primarySeries * dIpDvp - 1.0f;
      double a01 = mOverDt * dIaDva;
      double a02 = -mOverDt * dIbDvb;
      double a10 = mOverDt * dIpDvp;
      double a11 = secondaryASeries * dIaDva - 1.0f;
      double a12 = -mabOverDt * dIbDvb;
      double a20 = -mOverDt * dIpDvp;
      double a21 = -mabOverDt * dIaDva;
      double a22 = secondaryBSeries * dIbDvb - 1.0f;

      double rhs[3] = {-f0, -f1, -f2};
      double delta[3] = {};
      bool solved =
          solveLinear3x3Direct(a00, a01, a02, a10, a11, a12, a20, a21, a22, rhs, delta);
      assert(solved && "interstage 3x3 solve failed");
      (void)solved;

      primaryVoltage += delta[0];
      secondaryAVoltage += delta[1];
      secondaryBVoltage += delta[2];

      double maxDelta = std::max(std::fabs(delta[0]),
                                 std::max(std::fabs(delta[1]), std::fabs(delta[2])));
      assert(std::isfinite(primaryVoltage));
      assert(std::isfinite(secondaryAVoltage));
      assert(std::isfinite(secondaryBVoltage));
      if (maxDelta < kInterstageConvergenceTolerance) break;
    }

    double driverPlateVolts = driverPlateQuiescent - primaryVoltage;
    driverEvalCount++;
    driverPlateCurrentAbs =
        evaluateTriodePlateFast(controlGridVolts, driverPlateVolts, power.tubeTriodeModel,
                                power.tubeTriodeLut, driverTriodeLut)
            .currentAmps;
    primaryCurrent = driverPlateCurrentAbs - driverQuiescentCurrent -
                     coreLossConductance * primaryVoltage;
    secondaryACurrent =
        -evaluateTubeGridBranch(secondaryAVoltage, power.outputTubeBiasVolts,
                                gridLeakConductance, gridCurrentConductance)
             .current;
    secondaryBCurrent =
        -evaluateTubeGridBranch(secondaryBVoltage, power.outputTubeBiasVolts,
                                gridLeakConductance, gridCurrentConductance)
             .current;
  }

  DriverInterstageCenterTappedResult result{};
  result.driverPlateCurrentAbs = static_cast<float>(driverPlateCurrentAbs);
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
  return result;
}

}  // namespace

}  // namespace power_stage_internal

DriverInterstageCenterTappedResult solveDriverInterstageCenterTappedNoCap(
    const CurrentDrivenTransformer& transformer,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts,
    float driverPlateQuiescent,
    float driverQuiescentCurrent) {
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
            driverQuiescentCurrent, heuristicSubsteps);
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
            driverQuiescentCurrent, currentAdaptiveSubsteps);
    DriverInterstageCenterTappedResult coarse =
        power_stage_internal::solveDriverInterstageCenterTappedNoCapAtSubsteps(
            transformer, power, controlGridVolts, driverPlateQuiescent,
            driverQuiescentCurrent, trialSubsteps);
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
          driverQuiescentCurrent, currentAdaptiveSubsteps);
  steady.suggestedSubsteps = currentAdaptiveSubsteps;
  steady.suggestedValidationCountdown =
      std::max(currentValidationCountdown - 1, 0);
  return steady;
}
