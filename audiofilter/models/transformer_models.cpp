#include "transformer_models.h"

#include "../math/linear_solvers.h"
#include "../math/signal_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>

namespace {

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
  zero.clearState();
  p.slope =
      zero.projectStep(1.0f, secondaryLoadResistanceOhms,
                       primaryLoadResistanceOhms);

  for (int col = 0; col < 4; ++col) {
    CurrentDrivenTransformer basis = t;
    basis.clearState();
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
        basis.projectStep(0.0f, secondaryLoadResistanceOhms,
                          primaryLoadResistanceOhms);
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
  auto s0 =
      t.projectStep(0.0f, secondaryLoadResistanceOhms, primaryLoadResistanceOhms);
  auto s1 =
      t.projectStep(1.0f, secondaryLoadResistanceOhms, primaryLoadResistanceOhms);

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

void CurrentDrivenTransformer::setModel(
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
  clearState();
}

void CurrentDrivenTransformer::clearState() {
  primaryCurrent = 0.0f;
  secondaryCurrent = 0.0f;
  primaryVoltage = 0.0f;
  secondaryVoltage = 0.0f;
}

CurrentDrivenTransformerSample CurrentDrivenTransformer::projectStep(
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

CurrentDrivenTransformerSample CurrentDrivenTransformer::step(
    float primaryDriveCurrentAmps,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) {
  CurrentDrivenTransformerSample projected =
      projectStep(primaryDriveCurrentAmps, secondaryLoadResistanceOhms,
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
