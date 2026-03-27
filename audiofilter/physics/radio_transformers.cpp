#include "radio_transformers.h"

#include "../math/radio_linear_solvers.h"
#include "../math/radio_math.h"
#include "radio_tube_models.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace {

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

CurrentDrivenTransformerSample projectNoShuntCap(
    const CurrentDrivenTransformer& transformer,
    float primaryDriveCurrentAmps,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) {
  float dt = requirePositiveFinite(transformer.dtSub);
  float Rp = transformer.primaryResistanceOhms;
  float Rs = transformer.secondaryResistanceOhms;
  float Lp = transformer.cachedPrimaryInductance;
  float Ls = transformer.cachedSecondaryInductance;
  float M = transformer.cachedMutualInductance;
  float coreLossConductance =
      (transformer.primaryCoreLossResistanceOhms > 0.0f)
          ? 1.0f / transformer.primaryCoreLossResistanceOhms
          : 0.0f;
  float primaryLoadConductance =
      (primaryLoadResistanceOhms > 0.0f &&
       std::isfinite(primaryLoadResistanceOhms))
          ? 1.0f / primaryLoadResistanceOhms
          : 0.0f;
  float Gp = coreLossConductance + primaryLoadConductance;
  float Gs = (secondaryLoadResistanceOhms > 0.0f &&
              std::isfinite(secondaryLoadResistanceOhms))
                 ? 1.0f / secondaryLoadResistanceOhms
                 : 0.0f;
  float projectedPrimaryCurrent = transformer.primaryCurrent;
  float projectedSecondaryCurrent = transformer.secondaryCurrent;
  float projectedPrimaryVoltage = transformer.primaryVoltage;
  float projectedSecondaryVoltage = transformer.secondaryVoltage;
  float lpOverDt = Lp / dt;
  float lsOverDt = Ls / dt;
  float mOverDt = M / dt;
  float a11 = Rp + lpOverDt;
  float a12 = mOverDt;
  float a21 = mOverDt;
  float a22 = Rs + lsOverDt;
  bool primaryConductive = Gp > 0.0f;
  bool secondaryConductive = Gs > 0.0f;
  float invGp = primaryConductive ? 1.0f / Gp : 0.0f;
  float invGs = secondaryConductive ? 1.0f / Gs : 0.0f;

  for (int step = 0; step < transformer.integrationSubsteps; ++step) {
    float ipPrev = projectedPrimaryCurrent;
    float isPrev = projectedSecondaryCurrent;
    float c1 = lpOverDt * ipPrev + mOverDt * isPrev;
    float c2 = mOverDt * ipPrev + lsOverDt * isPrev;

    if (!primaryConductive) {
      projectedPrimaryCurrent = primaryDriveCurrentAmps;
      if (!secondaryConductive) {
        projectedSecondaryCurrent = 0.0f;
      } else {
        float denom = a22 + invGs;
        assert(std::fabs(denom) >= 1e-12f);
        projectedSecondaryCurrent =
            (c2 - a21 * projectedPrimaryCurrent) / denom;
      }
    } else if (!secondaryConductive) {
      projectedSecondaryCurrent = 0.0f;
      float denom = a11 + invGp;
      assert(std::fabs(denom) >= 1e-12f);
      projectedPrimaryCurrent = (c1 + primaryDriveCurrentAmps * invGp) / denom;
    } else {
      float det = (a11 + invGp) * (a22 + invGs) - a12 * a21;
      assert(std::fabs(det) >= 1e-12f);
      float rhs0 = c1 + primaryDriveCurrentAmps * invGp;
      float rhs1 = c2;
      projectedPrimaryCurrent = (rhs0 * (a22 + invGs) - a12 * rhs1) / det;
      projectedSecondaryCurrent =
          ((a11 + invGp) * rhs1 - rhs0 * a21) / det;
    }

    projectedPrimaryVoltage =
        primaryConductive
            ? ((primaryDriveCurrentAmps - projectedPrimaryCurrent) * invGp)
            : (a11 * projectedPrimaryCurrent +
               a12 * projectedSecondaryCurrent - c1);
    projectedSecondaryVoltage =
        secondaryConductive
            ? (-projectedSecondaryCurrent * invGs)
            : (a21 * projectedPrimaryCurrent +
               a22 * projectedSecondaryCurrent - c2);
    assert(std::isfinite(projectedPrimaryCurrent) &&
           std::isfinite(projectedSecondaryCurrent) &&
           std::isfinite(projectedPrimaryVoltage) &&
           std::isfinite(projectedSecondaryVoltage));
  }

  return CurrentDrivenTransformerSample{
      projectedPrimaryVoltage, projectedSecondaryVoltage,
      projectedPrimaryCurrent, projectedSecondaryCurrent};
}

float transformerStateComponent(const CurrentDrivenTransformerSample& s,
                                int index) {
  switch (index) {
    case 0:
      return s.primaryCurrent;
    case 1:
      return s.secondaryCurrent;
    case 2:
      return s.primaryVoltage;
    default:
      return s.secondaryVoltage;
  }
}

}  // namespace

CurrentDrivenTransformerSample evalFixedLoadAffineBase(
    const std::array<float, 16>& stateA,
    const CurrentDrivenTransformer& t) {
  float state[4] = {t.primaryCurrent, t.secondaryCurrent, t.primaryVoltage,
                    t.secondaryVoltage};
  float next[4] = {};
  for (int row = 0; row < 4; ++row) {
    float sum = 0.0f;
    for (int col = 0; col < 4; ++col) {
      sum += stateA[row * 4 + col] * state[col];
    }
    next[row] = sum;
  }

  return CurrentDrivenTransformerSample{next[2], next[3], next[0], next[1]};
}

FixedLoadAffineTransformerProjection buildFixedLoadAffineProjection(
    const CurrentDrivenTransformer& t,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) {
  FixedLoadAffineTransformerProjection p{};

  CurrentDrivenTransformer zero = t;
  zero.reset();
  p.slope =
      zero.project(1.0f, secondaryLoadResistanceOhms, primaryLoadResistanceOhms);

  for (int col = 0; col < 4; ++col) {
    CurrentDrivenTransformer basis = t;
    basis.reset();
    switch (col) {
      case 0:
        basis.primaryCurrent = 1.0f;
        break;
      case 1:
        basis.secondaryCurrent = 1.0f;
        break;
      case 2:
        basis.primaryVoltage = 1.0f;
        break;
      case 3:
        basis.secondaryVoltage = 1.0f;
        break;
    }
    CurrentDrivenTransformerSample out =
        basis.project(0.0f, secondaryLoadResistanceOhms, primaryLoadResistanceOhms);
    for (int row = 0; row < 4; ++row) {
      p.stateA[row * 4 + col] = transformerStateComponent(out, row);
    }
  }

  return p;
}

AffineTransformerProjection buildAffineProjection(
    const CurrentDrivenTransformer& t,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) {
  auto s0 = t.project(0.0f, secondaryLoadResistanceOhms, primaryLoadResistanceOhms);
  auto s1 = t.project(1.0f, secondaryLoadResistanceOhms, primaryLoadResistanceOhms);

  AffineTransformerProjection p{};
  p.base = s0;
  p.slope.primaryVoltage = s1.primaryVoltage - s0.primaryVoltage;
  p.slope.secondaryVoltage = s1.secondaryVoltage - s0.secondaryVoltage;
  p.slope.primaryCurrent = s1.primaryCurrent - s0.primaryCurrent;
  p.slope.secondaryCurrent = s1.secondaryCurrent - s0.secondaryCurrent;
  return p;
}

CurrentDrivenTransformerSample evalAffineProjection(
    const AffineTransformerProjection& p,
    float driveCurrent) {
  CurrentDrivenTransformerSample s{};
  s.primaryVoltage = p.base.primaryVoltage + driveCurrent * p.slope.primaryVoltage;
  s.secondaryVoltage =
      p.base.secondaryVoltage + driveCurrent * p.slope.secondaryVoltage;
  s.primaryCurrent = p.base.primaryCurrent + driveCurrent * p.slope.primaryCurrent;
  s.secondaryCurrent =
      p.base.secondaryCurrent + driveCurrent * p.slope.secondaryCurrent;
  return s;
}

float solveOutputPrimaryVoltageAffine(const AffineTransformerProjection& proj,
                                      const Radio1938::PowerNodeState& power,
                                      float outputPlateQuiescent,
                                      float gridA,
                                      float gridB,
                                      float initialVp) {
  float vp = initialVp;

  for (int iter = 0; iter < 8; ++iter) {
    float plateA = outputPlateQuiescent - 0.5f * vp;
    float plateB = outputPlateQuiescent + 0.5f * vp;
    KorenTriodePlateEval evalA = evaluateKorenTriodePlateRuntime(
        power.outputTubeBiasVolts + gridA, plateA, power.outputTubeTriodeModel,
        power.outputTubeTriodeLut);
    KorenTriodePlateEval evalB = evaluateKorenTriodePlateRuntime(
        power.outputTubeBiasVolts + gridB, plateB, power.outputTubeTriodeModel,
        power.outputTubeTriodeLut);

    float drive =
        0.5f * static_cast<float>(evalA.currentAmps - evalB.currentAmps);
    float driveSlope = -0.25f * static_cast<float>(evalA.conductanceSiemens +
                                                   evalB.conductanceSiemens);

    float f =
        proj.base.primaryVoltage + proj.slope.primaryVoltage * drive - vp;
    float df = proj.slope.primaryVoltage * driveSlope - 1.0f;
    assert(std::isfinite(df) && std::fabs(df) >= 1e-9f);

    vp -= f / df;
    assert(std::isfinite(vp));
  }

  return vp;
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
    transformer.reset();
    double sumSq = 0.0;
    int sampleCount = 0;

    for (int cycle = 0; cycle < (kSettleCycles + kMeasureCycles); ++cycle) {
      for (int i = 0; i < kSamplesPerCycle; ++i) {
        float phase = kRadioTwoPi * static_cast<float>(i) /
                      static_cast<float>(kSamplesPerCycle);
        float gridA = gridPeak * std::sin(phase);
        float gridB = -gridA;
        AffineTransformerProjection affineOut =
            buildAffineProjection(transformer, loadResistance, 0.0f);
        float solvedPrimaryVoltage = solveOutputPrimaryVoltageAffine(
            affineOut, power, outputPlateQuiescent, gridA, gridB,
            transformer.primaryVoltage);
        float plateA = outputPlateQuiescent - 0.5f * solvedPrimaryVoltage;
        float plateB = outputPlateQuiescent + 0.5f * solvedPrimaryVoltage;
        KorenTriodePlateEval evalA = evaluateKorenTriodePlateRuntime(
            power.outputTubeBiasVolts + gridA, plateA,
            power.outputTubeTriodeModel, power.outputTubeTriodeLut);
        KorenTriodePlateEval evalB = evaluateKorenTriodePlateRuntime(
            power.outputTubeBiasVolts + gridB, plateB,
            power.outputTubeTriodeModel, power.outputTubeTriodeLut);
        float driveCurrent =
            0.5f * static_cast<float>(evalA.currentAmps - evalB.currentAmps);
        auto outputSample = transformer.process(driveCurrent, loadResistance, 0.0f);
        if (cycle >= kSettleCycles) {
          sumSq += static_cast<double>(outputSample.secondaryVoltage) *
                   static_cast<double>(outputSample.secondaryVoltage);
          sampleCount++;
        }
      }
    }

    float secondaryRms = std::sqrt(sumSq / std::max(sampleCount, 1));
    bestSecondaryRms = std::max(bestSecondaryRms, secondaryRms);
  }

  return (bestSecondaryRms * bestSecondaryRms) / loadResistance;
}

DriverInterstageCenterTappedResult solveDriverInterstageCenterTappedNoCap(
    const CurrentDrivenTransformer& t,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts,
    float driverPlateQuiescent,
    float driverQuiescentCurrent) {
  assert(power.tubeTriodeConnected);

  float dt = requirePositiveFinite(t.dtSub);
  float nTotal = requirePositiveFinite(t.cachedTurns);
  float nHalf = 2.0f * nTotal;

  float Rp = t.primaryResistanceOhms;
  float Ra = 0.5f * t.secondaryResistanceOhms;
  float Rb = 0.5f * t.secondaryResistanceOhms;

  float Lp = t.cachedPrimaryInductance;
  float LlkHalf = 0.5f * t.secondaryLeakageInductanceHenries;
  float LhalfMag = t.magnetizingInductanceHenries / (nHalf * nHalf);
  float La = LlkHalf + LhalfMag;
  float Lb = LlkHalf + LhalfMag;
  float Mab = LhalfMag;
  float M = 0.5f * t.cachedMutualInductance;

  float coreLossConductance =
      (t.primaryCoreLossResistanceOhms > 0.0f)
          ? (1.0f / t.primaryCoreLossResistanceOhms)
          : 0.0f;

  float vp = power.interstageCt.primaryVoltage;
  float va = power.interstageCt.secondaryAVoltage;
  float vb = power.interstageCt.secondaryBVoltage;
  float ip = power.interstageCt.primaryCurrent;
  float ia = power.interstageCt.secondaryACurrent;
  float ib = power.interstageCt.secondaryBCurrent;
  float lpOverDt = Lp / dt;
  float laOverDt = La / dt;
  float lbOverDt = Lb / dt;
  float mOverDt = M / dt;
  float mabOverDt = Mab / dt;

  for (int step = 0; step < t.integrationSubsteps; ++step) {
    float ipPrev = ip;
    float iaPrev = ia;
    float ibPrev = ib;
    float cPrimary = lpOverDt * ipPrev + mOverDt * iaPrev - mOverDt * ibPrev;
    float cSecondaryA = mOverDt * ipPrev + laOverDt * iaPrev - mabOverDt * ibPrev;
    float cSecondaryB =
        -mOverDt * ipPrev - mabOverDt * iaPrev + lbOverDt * ibPrev;

    for (int iter = 0; iter < 12; ++iter) {
      float driverPlateVolts = driverPlateQuiescent - vp;
      KorenTriodePlateEval driverEval = evaluateKorenTriodePlateRuntime(
          controlGridVolts, driverPlateVolts, power.tubeTriodeModel,
          power.tubeTriodeLut);
      float driverPlateCurrentAbs = static_cast<float>(driverEval.currentAmps);
      float dIdrive_dVp = -static_cast<float>(driverEval.conductanceSiemens);
      float idrive = driverPlateCurrentAbs - driverQuiescentCurrent;
      float ipNow = idrive - coreLossConductance * vp;

      float iaBranch = tubeGridBranchCurrent(
          va, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float iaNow = -iaBranch;
      float dIa_dVa = -tubeGridBranchSlope(
          va, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float ibBranch = tubeGridBranchCurrent(
          vb, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float ibNow = -ibBranch;
      float dIb_dVb = -tubeGridBranchSlope(
          vb, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
          power.outputGridCurrentResistanceOhms);
      float dIp_dVp = dIdrive_dVp - coreLossConductance;

      float f[3] = {
          (Rp + lpOverDt) * ipNow + mOverDt * iaNow - mOverDt * ibNow - vp -
              cPrimary,
          mOverDt * ipNow + (Ra + laOverDt) * iaNow - mabOverDt * ibNow - va -
              cSecondaryA,
          -mOverDt * ipNow - mabOverDt * iaNow + (Rb + lbOverDt) * ibNow - vb -
              cSecondaryB,
      };

      float j[3][3] = {
          {(Rp + lpOverDt) * dIp_dVp - 1.0f, mOverDt * dIa_dVa,
           -mOverDt * dIb_dVb},
          {mOverDt * dIp_dVp, (Ra + laOverDt) * dIa_dVa - 1.0f,
           -mabOverDt * dIb_dVb},
          {-mOverDt * dIp_dVp, -mabOverDt * dIa_dVa,
           (Rb + lbOverDt) * dIb_dVb - 1.0f},
      };

      float rhs[3] = {-f[0], -f[1], -f[2]};
      float delta[3] = {};
      if (!solveLinear3x3Direct(j[0][0], j[0][1], j[0][2], j[1][0], j[1][1],
                                j[1][2], j[2][0], j[2][1], j[2][2], rhs,
                                delta)) {
        assert(false && "interstage 3x3 solve failed");
      }

      vp += delta[0];
      va += delta[1];
      vb += delta[2];

      float maxDelta = std::max(
          std::fabs(delta[0]),
          std::max(std::fabs(delta[1]), std::fabs(delta[2])));

      if (maxDelta < 1e-6f) {
        break;
      }

      assert(std::isfinite(vp));
      assert(std::isfinite(va));
      assert(std::isfinite(vb));
    }

    float driverPlateVolts = driverPlateQuiescent - vp;
    float driverPlateCurrentAbs = static_cast<float>(
        evaluateKorenTriodePlateRuntime(controlGridVolts, driverPlateVolts,
                                        power.tubeTriodeModel,
                                        power.tubeTriodeLut)
            .currentAmps);
    ip = driverPlateCurrentAbs - driverQuiescentCurrent -
         coreLossConductance * vp;
    ia = -tubeGridBranchCurrent(
        va, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
        power.outputGridCurrentResistanceOhms);
    ib = -tubeGridBranchCurrent(
        vb, power.outputTubeBiasVolts, power.outputGridLeakResistanceOhms,
        power.outputGridCurrentResistanceOhms);
  }

  float finalDriverPlateVolts = driverPlateQuiescent - vp;
  float finalDriverPlateCurrentAbs = static_cast<float>(
      evaluateKorenTriodePlateRuntime(controlGridVolts, finalDriverPlateVolts,
                                      power.tubeTriodeModel,
                                      power.tubeTriodeLut)
          .currentAmps);

  DriverInterstageCenterTappedResult out{};
  out.driverPlateCurrentAbs = finalDriverPlateCurrentAbs;
  out.primaryCurrent = ip;
  out.primaryVoltage = vp;
  out.secondaryACurrent = ia;
  out.secondaryAVoltage = va;
  out.secondaryBCurrent = ib;
  out.secondaryBVoltage = vb;
  return out;
}

void CurrentDrivenTransformer::configure(
    float newFs,
    float newPrimaryLeakageInductanceHenries,
    float newMagnetizingInductanceHenries,
    float newTurnsRatioPrimaryToSecondary,
    float newPrimaryResistanceOhms,
    float newPrimaryCoreLossResistanceOhms,
    float newPrimaryShuntCapFarads,
    float newSecondaryLeakageInductanceHenries,
    float newSecondaryResistanceOhms,
    float newSecondaryShuntCapFarads,
    int newIntegrationSubsteps) {
  assert(std::isfinite(newFs) && newFs > 0.0f);
  assert(std::isfinite(newPrimaryLeakageInductanceHenries) &&
         newPrimaryLeakageInductanceHenries >= 0.0f);
  assert(std::isfinite(newMagnetizingInductanceHenries) &&
         newMagnetizingInductanceHenries > 0.0f);
  assert(std::isfinite(newTurnsRatioPrimaryToSecondary) &&
         newTurnsRatioPrimaryToSecondary > 0.0f);
  assert(std::isfinite(newPrimaryResistanceOhms) &&
         newPrimaryResistanceOhms >= 0.0f);
  assert(std::isfinite(newPrimaryCoreLossResistanceOhms) &&
         newPrimaryCoreLossResistanceOhms >= 0.0f);
  assert(std::isfinite(newPrimaryShuntCapFarads) &&
         newPrimaryShuntCapFarads >= 0.0f);
  assert(std::isfinite(newSecondaryLeakageInductanceHenries) &&
         newSecondaryLeakageInductanceHenries >= 0.0f);
  assert(std::isfinite(newSecondaryResistanceOhms) &&
         newSecondaryResistanceOhms >= 0.0f);
  assert(std::isfinite(newSecondaryShuntCapFarads) &&
         newSecondaryShuntCapFarads >= 0.0f);
  assert(newIntegrationSubsteps > 0);
  fs = newFs;
  primaryLeakageInductanceHenries = newPrimaryLeakageInductanceHenries;
  magnetizingInductanceHenries = newMagnetizingInductanceHenries;
  turnsRatioPrimaryToSecondary = newTurnsRatioPrimaryToSecondary;
  primaryResistanceOhms = newPrimaryResistanceOhms;
  primaryCoreLossResistanceOhms = newPrimaryCoreLossResistanceOhms;
  primaryShuntCapFarads = newPrimaryShuntCapFarads;
  secondaryLeakageInductanceHenries = newSecondaryLeakageInductanceHenries;
  secondaryResistanceOhms = newSecondaryResistanceOhms;
  secondaryShuntCapFarads = newSecondaryShuntCapFarads;
  integrationSubsteps = newIntegrationSubsteps;
  dtSub = 1.0f / (fs * static_cast<float>(integrationSubsteps));
  cachedTurns = turnsRatioPrimaryToSecondary;
  cachedPrimaryInductance =
      primaryLeakageInductanceHenries + magnetizingInductanceHenries;
  cachedSecondaryInductance =
      secondaryLeakageInductanceHenries +
      magnetizingInductanceHenries / (cachedTurns * cachedTurns);
  cachedMutualInductance = magnetizingInductanceHenries / cachedTurns;
  reset();
}

void CurrentDrivenTransformer::reset() {
  primaryCurrent = 0.0f;
  secondaryCurrent = 0.0f;
  primaryVoltage = 0.0f;
  secondaryVoltage = 0.0f;
}

CurrentDrivenTransformerSample CurrentDrivenTransformer::project(
    float primaryDriveCurrentAmps,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) const {
  if (primaryShuntCapFarads <= 0.0f && secondaryShuntCapFarads <= 0.0f) {
    return projectNoShuntCap(*this, primaryDriveCurrentAmps,
                             secondaryLoadResistanceOhms,
                             primaryLoadResistanceOhms);
  }

  float dt = requirePositiveFinite(dtSub);
  float primaryInductance = cachedPrimaryInductance;
  float secondaryInductance = cachedSecondaryInductance;
  float mutualInductance = cachedMutualInductance;
  float primaryCap = requirePositiveFinite(primaryShuntCapFarads);
  float secondaryCap = requirePositiveFinite(secondaryShuntCapFarads);
  float primaryCoreConductance =
      (primaryCoreLossResistanceOhms > 0.0f)
          ? 1.0f / primaryCoreLossResistanceOhms
          : 0.0f;
  if (primaryLoadResistanceOhms > 0.0f &&
      std::isfinite(primaryLoadResistanceOhms)) {
    primaryCoreConductance += 1.0f / primaryLoadResistanceOhms;
  }
  float secondaryLoadConductance =
      (secondaryLoadResistanceOhms > 0.0f &&
       std::isfinite(secondaryLoadResistanceOhms))
          ? 1.0f / secondaryLoadResistanceOhms
          : 0.0f;
  float determinant = primaryInductance * secondaryInductance -
                      mutualInductance * mutualInductance;
  assert(determinant > 0.0f);
  float a11 = secondaryInductance / determinant;
  float a12 = -mutualInductance / determinant;
  float a21 = -mutualInductance / determinant;
  float a22 = primaryInductance / determinant;
  float projectedPrimaryCurrent = primaryCurrent;
  float projectedSecondaryCurrent = secondaryCurrent;
  float projectedPrimaryVoltage = primaryVoltage;
  float projectedSecondaryVoltage = secondaryVoltage;
  constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

  for (int step = 0; step < integrationSubsteps; ++step) {
    float system[4][4] = {
        {1.0f + 0.5f * dt * a11 * primaryResistanceOhms,
         0.5f * dt * a12 * secondaryResistanceOhms, -0.5f * dt * a11,
         -0.5f * dt * a12},
        {0.5f * dt * a21 * primaryResistanceOhms,
         1.0f + 0.5f * dt * a22 * secondaryResistanceOhms, -0.5f * dt * a21,
         -0.5f * dt * a22},
        {0.5f * dt / primaryCap, 0.0f,
         1.0f + 0.5f * dt * primaryCoreConductance / primaryCap, 0.0f},
        {0.0f, 0.5f * dt / secondaryCap, 0.0f,
         1.0f + 0.5f * dt * secondaryLoadConductance / secondaryCap},
    };
    float b[4] = {
        (1.0f - 0.5f * dt * a11 * primaryResistanceOhms) *
                projectedPrimaryCurrent -
            0.5f * dt * a12 * secondaryResistanceOhms *
                projectedSecondaryCurrent +
            0.5f * dt * a11 * projectedPrimaryVoltage +
            0.5f * dt * a12 * projectedSecondaryVoltage,
        -0.5f * dt * a21 * primaryResistanceOhms * projectedPrimaryCurrent +
            (1.0f - 0.5f * dt * a22 * secondaryResistanceOhms) *
                projectedSecondaryCurrent +
            0.5f * dt * a21 * projectedPrimaryVoltage +
            0.5f * dt * a22 * projectedSecondaryVoltage,
        -0.5f * dt / primaryCap * projectedPrimaryCurrent +
            (1.0f - 0.5f * dt * primaryCoreConductance / primaryCap) *
                projectedPrimaryVoltage +
            dt * (primaryDriveCurrentAmps / primaryCap),
        -0.5f * dt / secondaryCap * projectedSecondaryCurrent +
            (1.0f - 0.5f * dt * secondaryLoadConductance / secondaryCap) *
                projectedSecondaryVoltage,
    };
    float x[4] = {kNaN, kNaN, kNaN, kNaN};
    bool solved = solveLinear4x4(system, b, x);
    assert(solved);
    projectedPrimaryCurrent = x[0];
    projectedSecondaryCurrent = x[1];
    projectedPrimaryVoltage = x[2];
    projectedSecondaryVoltage = x[3];
    assert(std::isfinite(projectedPrimaryCurrent) &&
           std::isfinite(projectedSecondaryCurrent) &&
           std::isfinite(projectedPrimaryVoltage) &&
           std::isfinite(projectedSecondaryVoltage));
  }

  return {projectedPrimaryVoltage, projectedSecondaryVoltage,
          projectedPrimaryCurrent, projectedSecondaryCurrent};
}

CurrentDrivenTransformerSample CurrentDrivenTransformer::process(
    float primaryDriveCurrentAmps,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) {
  CurrentDrivenTransformerSample projected =
      project(primaryDriveCurrentAmps, secondaryLoadResistanceOhms,
              primaryLoadResistanceOhms);
  assert(std::isfinite(projected.primaryVoltage) &&
         std::isfinite(projected.secondaryVoltage) &&
         std::isfinite(projected.primaryCurrent) &&
         std::isfinite(projected.secondaryCurrent));
  primaryVoltage = projected.primaryVoltage;
  secondaryVoltage = projected.secondaryVoltage;
  primaryCurrent = projected.primaryCurrent;
  secondaryCurrent = projected.secondaryCurrent;
  return projected;
}
