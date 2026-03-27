#include "radio_tube_models.h"

#include "../math/radio_linear_solvers.h"
#include "../math/radio_math.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <limits>
#include <vector>

float FixedPlatePentodeEvaluator::eval(float gridVolts) const {
  float activation =
      std::max(softplusVolts(gridVolts - cutoffVolts, gridSoftnessVolts), 0.0f);
  return currentScale * std::pow(activation, exponent);
}

float FixedPlatePentodeEvaluator::evalDerivative(float gridVolts) const {
  float slope = requirePositiveFinite(gridSoftnessVolts);
  float y = (gridVolts - cutoffVolts) / slope;
  float activation = 0.0f;
  float logistic = 0.0f;
  if (y >= 20.0f) {
    activation = gridVolts - cutoffVolts;
    logistic = 1.0f;
  } else if (y <= -20.0f) {
    activation = 0.0f;
    logistic = 0.0f;
  } else {
    activation = slope * std::log1p(std::exp(y));
    logistic = 1.0f / (1.0f + std::exp(-y));
  }
  if (activation <= 0.0f) {
    return 0.0f;
  }
  return currentScale * exponent * std::pow(activation, exponent - 1.0f) *
         logistic;
}

namespace {

template <typename Fn, typename T>
T finiteDifferenceDerivative(Fn&& fn, T x, T delta) {
  T safeDelta = requirePositiveFinite(delta);
  T high = fn(x + safeDelta);
  T low = fn(x - safeDelta);
  return (high - low) / (static_cast<T>(2.0) * safeDelta);
}

KorenTriodeModel makeKorenTriodeModel(double mu,
                                      double kg1,
                                      double kp,
                                      double kvb) {
  assert(std::isfinite(mu) && mu > 0.0);
  assert(std::isfinite(kg1) && kg1 > 0.0);
  assert(std::isfinite(kp) && kp > 0.0);
  assert(std::isfinite(kvb) && kvb > 0.0);

  KorenTriodeModel m{};
  m.mu = mu;
  m.ex = 1.5;
  m.kg1 = kg1;
  m.kp = kp;
  m.kvb = kvb;
  return m;
}

double korenTriodePlateCurrent(double vgk,
                               double vpk,
                               const KorenTriodeModel& m);
double korenTriodePlateConductance(double vgk,
                                   double vpk,
                                   const KorenTriodeModel& m);
KorenTriodePlateEval evaluateKorenTriodePlate(double vgk,
                                              double vpk,
                                              const KorenTriodeModel& m);

float deviceTubePlateCurrent(float gridVolts,
                             float plateVolts,
                             float screenVolts,
                             float biasVolts,
                             float cutoffVolts,
                             float quiescentPlateVolts,
                             float quiescentScreenVolts,
                             float quiescentPlateCurrentAmps,
                             float mutualConductanceSiemens,
                             float plateKneeVolts,
                             float gridSoftnessVolts);

struct KorenTriodeMetrics {
  double currentAmps = 0.0;
  double gmSiemens = 0.0;
  double gpSiemens = 0.0;
};

KorenTriodeMetrics evaluateKorenTriodeMetrics(double vgkQ,
                                              double vpkQ,
                                              const KorenTriodeModel& m) {
  KorenTriodeMetrics metrics{};
  KorenTriodePlateEval plate = evaluateKorenTriodePlate(vgkQ, vpkQ, m);
  metrics.currentAmps = plate.currentAmps;
  metrics.gmSiemens = finiteDifferenceDerivative(
      [&](double vgk) { return korenTriodePlateCurrent(vgk, vpkQ, m); }, vgkQ,
      0.01);
  metrics.gpSiemens = plate.conductanceSiemens;
  return metrics;
}

KorenTriodeModel fitKorenTriodeModel(double vgkQ,
                                     double vpkQ,
                                     double iqTarget,
                                     double gmTarget,
                                     double mu) {
  assert(std::isfinite(vgkQ));
  assert(std::isfinite(vpkQ) && vpkQ > 0.0);
  assert(std::isfinite(iqTarget) && iqTarget > 0.0);
  assert(std::isfinite(gmTarget) && gmTarget > 0.0);
  assert(std::isfinite(mu) && mu > 0.0);

  double gpTarget = gmTarget / mu;
  double kpInit = 100.0;
  double kvbInit = vpkQ * vpkQ;
  KorenTriodeModel initShape = makeKorenTriodeModel(mu, 1.0, kpInit, kvbInit);
  double initCurrent = korenTriodePlateCurrent(vgkQ, vpkQ, initShape);
  double kg1Init =
      (initCurrent > 0.0)
          ? (initCurrent / iqTarget)
          : (std::pow(vpkQ, initShape.ex) / iqTarget);

  double u[3] = {std::log(kg1Init), std::log(kpInit), std::log(kvbInit)};
  double bestU[3] = {u[0], u[1], u[2]};
  double bestNorm = std::numeric_limits<double>::infinity();
  const double kModelLogMin = std::log(1e-12);
  const double kModelLogMax = std::log(1e12);

  auto clampModelLog = [&](double x) {
    return std::clamp(x, kModelLogMin, kModelLogMax);
  };

  auto evalResiduals = [&](const double params[3], double residuals[3]) {
    double safeParams[3] = {clampModelLog(params[0]), clampModelLog(params[1]),
                            clampModelLog(params[2])};
    KorenTriodeModel model = makeKorenTriodeModel(
        mu, std::exp(safeParams[0]), std::exp(safeParams[1]),
        std::exp(safeParams[2]));
    KorenTriodeMetrics metrics = evaluateKorenTriodeMetrics(vgkQ, vpkQ, model);
    residuals[0] = metrics.currentAmps / iqTarget - 1.0;
    residuals[1] = metrics.gmSiemens / gmTarget - 1.0;
    residuals[2] = metrics.gpSiemens / gpTarget - 1.0;
  };

  for (int iter = 0; iter < 16; ++iter) {
    double f[3] = {};
    evalResiduals(u, f);
    double norm = std::fabs(f[0]) + std::fabs(f[1]) + std::fabs(f[2]);
    if (norm < bestNorm) {
      bestNorm = norm;
      bestU[0] = u[0];
      bestU[1] = u[1];
      bestU[2] = u[2];
    }
    if (norm < 1e-8) break;

    float a[3][3] = {};
    float b[3] = {-static_cast<float>(f[0]), -static_cast<float>(f[1]),
                  -static_cast<float>(f[2])};
    for (int col = 0; col < 3; ++col) {
      double up[3] = {u[0], u[1], u[2]};
      double um[3] = {u[0], u[1], u[2]};
      up[col] += 1e-3;
      um[col] -= 1e-3;
      double fp[3] = {};
      double fm[3] = {};
      evalResiduals(up, fp);
      evalResiduals(um, fm);
      for (int row = 0; row < 3; ++row) {
        a[row][col] = static_cast<float>((fp[row] - fm[row]) / 2e-3);
      }
    }

    float delta[3] = {};
    if (!solveLinear3x3(a, b, delta)) break;

    double lambda = 1.0;
    double candidateBestNorm = bestNorm;
    double candidateBestU[3] = {u[0], u[1], u[2]};
    for (int ls = 0; ls < 8; ++ls) {
      double candU[3] = {u[0] + lambda * delta[0], u[1] + lambda * delta[1],
                         u[2] + lambda * delta[2]};
      double candF[3] = {};
      evalResiduals(candU, candF);
      double candNorm =
          std::fabs(candF[0]) + std::fabs(candF[1]) + std::fabs(candF[2]);
      if (candNorm < candidateBestNorm) {
        candidateBestNorm = candNorm;
        candidateBestU[0] = candU[0];
        candidateBestU[1] = candU[1];
        candidateBestU[2] = candU[2];
      }
      lambda *= 0.5;
    }

    u[0] = clampModelLog(candidateBestU[0]);
    u[1] = clampModelLog(candidateBestU[1]);
    u[2] = clampModelLog(candidateBestU[2]);

    double maxDelta =
        std::max(std::fabs(delta[0]),
                 std::max(std::fabs(delta[1]), std::fabs(delta[2])));
    if (maxDelta < 1e-6) break;
  }

  return makeKorenTriodeModel(mu, std::exp(bestU[0]), std::exp(bestU[1]),
                              std::exp(bestU[2]));
}

float fitPentodeModelCutoffVolts(float biasVolts,
                                 float plateVolts,
                                 float screenVolts,
                                 float plateCurrentAmps,
                                 float mutualConductanceSiemens,
                                 float plateKneeVolts,
                                 float gridSoftnessVolts) {
  float safePlateVolts = std::max(plateVolts, 1.0f);
  float safeScreenVolts = std::max(screenVolts, 1.0f);
  float safePlateCurrent = std::max(plateCurrentAmps, 1e-6f);
  float searchMin = biasVolts - 80.0f;
  float searchMax = biasVolts + 6.0f;
  float bestCutoff = searchMin;
  float bestError = std::numeric_limits<float>::infinity();
  constexpr int kSteps = 256;
  for (int i = 0; i <= kSteps; ++i) {
    float t = static_cast<float>(i) / static_cast<float>(kSteps);
    float cutoffVolts = searchMin + (searchMax - searchMin) * t;
    float gmEstimate = finiteDifferenceDerivative(
        [&](float gridVolts) {
          return deviceTubePlateCurrent(
              gridVolts, safePlateVolts, safeScreenVolts, biasVolts,
              cutoffVolts, safePlateVolts, safeScreenVolts, safePlateCurrent,
              mutualConductanceSiemens, plateKneeVolts, gridSoftnessVolts);
        },
        biasVolts, 0.02f);
    float error = std::fabs(gmEstimate - mutualConductanceSiemens);
    if (error < bestError) {
      bestError = error;
      bestCutoff = cutoffVolts;
    }
  }
  return bestCutoff;
}

template <typename PlateCurrentFn>
float solveLoadLinePlateVoltage(float plateSupplyVolts,
                                float loadResistanceOhms,
                                float initialGuessVolts,
                                PlateCurrentFn&& plateCurrentForPlate) {
  float safeSupply = requirePositiveFinite(plateSupplyVolts);
  float safeLoadResistance = requirePositiveFinite(loadResistanceOhms);

  auto residual = [&](float plateVolts) {
    float safePlate = clampf(plateVolts, 0.0f, safeSupply);
    return safeSupply - plateCurrentForPlate(safePlate) * safeLoadResistance -
           safePlate;
  };

  float bestPlate = clampf(initialGuessVolts, 0.0f, safeSupply);
  float bestResidual = residual(bestPlate);
  float low = 0.0f;
  float high = safeSupply;
  float lowResidual = residual(low);
  float highResidual = residual(high);
  if (std::fabs(lowResidual) < std::fabs(bestResidual)) {
    bestPlate = low;
    bestResidual = lowResidual;
  }
  if (std::fabs(highResidual) < std::fabs(bestResidual)) {
    bestPlate = high;
    bestResidual = highResidual;
  }

  if (lowResidual * highResidual <= 0.0f) {
    for (int i = 0; i < 24; ++i) {
      float mid = 0.5f * (low + high);
      float midResidual = residual(mid);
      if (std::fabs(midResidual) < std::fabs(bestResidual)) {
        bestPlate = mid;
        bestResidual = midResidual;
      }
      if (midResidual == 0.0f) break;
      if (lowResidual * midResidual <= 0.0f) {
        high = mid;
        highResidual = midResidual;
      } else {
        low = mid;
        lowResidual = midResidual;
      }
    }
    return bestPlate;
  }

  float plateVolts = bestPlate;
  for (int i = 0; i < 16; ++i) {
    float targetPlate = safeSupply - plateCurrentForPlate(plateVolts) *
                                         safeLoadResistance;
    targetPlate = clampf(targetPlate, 0.0f, safeSupply);
    plateVolts = 0.5f * (plateVolts + targetPlate);
  }
  return plateVolts;
}

double korenTriodePlateCurrent(double vgk,
                               double vpk,
                               const KorenTriodeModel& m) {
  return evaluateKorenTriodePlate(vgk, vpk, m).currentAmps;
}

double korenTriodePlateConductance(double vgk,
                                   double vpk,
                                   const KorenTriodeModel& m) {
  return evaluateKorenTriodePlate(vgk, vpk, m).conductanceSiemens;
}

KorenTriodePlateEval evaluateKorenTriodePlate(double vgk,
                                              double vpk,
                                              const KorenTriodeModel& m) {
  KorenTriodePlateEval eval{};
  if (vpk <= 0.0) {
    return eval;
  }

  double sqrtTermSq = m.kvb + vpk * vpk;
  double sqrtTerm = std::sqrt(sqrtTermSq);
  double term = (1.0 / m.mu) + vgk / sqrtTerm;
  double activation = m.kp * term;
  double logTerm = stableLog1pExp(activation);
  double e1 = (vpk / m.kp) * logTerm;
  if (e1 <= 0.0) {
    return eval;
  }

  eval.currentAmps = std::pow(e1, m.ex) / m.kg1;

  double sigmoid = 0.0;
  if (activation >= 50.0) {
    sigmoid = 1.0;
  } else if (activation <= -50.0) {
    sigmoid = std::exp(activation);
  } else {
    sigmoid = 1.0 / (1.0 + std::exp(-activation));
  }

  double sqrtTermCubed = sqrtTermSq * sqrtTerm;
  double dE1dVpk =
      (logTerm / m.kp) -
      (vgk * vpk * vpk * sigmoid) / std::max(sqrtTermCubed, 1e-18);
  eval.conductanceSiemens =
      eval.currentAmps * (m.ex * dE1dVpk / std::max(e1, 1e-18));
  return eval;
}

void configureKorenTriodeLut(KorenTriodeLut& lut,
                             const KorenTriodeModel& model,
                             float vgkMin,
                             float vgkMax,
                             int vgkBins,
                             float vpkMin,
                             float vpkMax,
                             int vpkBins) {
  lut.vgkMin = vgkMin;
  lut.vgkMax = vgkMax;
  lut.vpkMin = vpkMin;
  lut.vpkMax = vpkMax;
  lut.vgkBins = std::max(vgkBins, 2);
  lut.vpkBins = std::max(vpkBins, 2);
  lut.vgkInvStep = static_cast<float>(lut.vgkBins - 1) /
                   std::max(lut.vgkMax - lut.vgkMin, 1e-6f);
  lut.vpkInvStep = static_cast<float>(lut.vpkBins - 1) /
                   std::max(lut.vpkMax - lut.vpkMin, 1e-6f);
  size_t sampleCount =
      static_cast<size_t>(lut.vgkBins) * static_cast<size_t>(lut.vpkBins);
  lut.currentAmps.resize(sampleCount);
  lut.conductanceSiemens.resize(sampleCount);

  for (int y = 0; y < lut.vpkBins; ++y) {
    float vpk = lut.vpkMin +
                static_cast<float>(y) /
                    static_cast<float>(lut.vpkBins - 1) *
                    (lut.vpkMax - lut.vpkMin);
    for (int x = 0; x < lut.vgkBins; ++x) {
      float vgk = lut.vgkMin +
                  static_cast<float>(x) /
                      static_cast<float>(lut.vgkBins - 1) *
                      (lut.vgkMax - lut.vgkMin);
      KorenTriodePlateEval eval = evaluateKorenTriodePlate(vgk, vpk, model);
      size_t index =
          static_cast<size_t>(y) * static_cast<size_t>(lut.vgkBins) +
          static_cast<size_t>(x);
      lut.currentAmps[index] = static_cast<float>(eval.currentAmps);
      lut.conductanceSiemens[index] =
          static_cast<float>(eval.conductanceSiemens);
    }
  }
}

KorenTriodePlateEval evaluateKorenTriodePlateLut(double vgk,
                                                 double vpk,
                                                 const KorenTriodeLut& lut) {
  if (!lut.valid()) {
    return {};
  }

  float clampedVgk =
      std::clamp(static_cast<float>(vgk), lut.vgkMin, lut.vgkMax);
  float clampedVpk =
      std::clamp(static_cast<float>(vpk), lut.vpkMin, lut.vpkMax);
  float x = (clampedVgk - lut.vgkMin) * lut.vgkInvStep;
  float y = (clampedVpk - lut.vpkMin) * lut.vpkInvStep;
  int x0 = std::clamp(static_cast<int>(x), 0, lut.vgkBins - 2);
  int y0 = std::clamp(static_cast<int>(y), 0, lut.vpkBins - 2);
  float tx = x - static_cast<float>(x0);
  float ty = y - static_cast<float>(y0);
  int x1 = x0 + 1;
  int y1 = y0 + 1;

  auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
  auto sampleAt = [&](const std::vector<float>& values, int xi, int yi) {
    size_t index =
        static_cast<size_t>(yi) * static_cast<size_t>(lut.vgkBins) +
        static_cast<size_t>(xi);
    return values[index];
  };

  float i00 = sampleAt(lut.currentAmps, x0, y0);
  float i10 = sampleAt(lut.currentAmps, x1, y0);
  float i01 = sampleAt(lut.currentAmps, x0, y1);
  float i11 = sampleAt(lut.currentAmps, x1, y1);
  float g00 = sampleAt(lut.conductanceSiemens, x0, y0);
  float g10 = sampleAt(lut.conductanceSiemens, x1, y0);
  float g01 = sampleAt(lut.conductanceSiemens, x0, y1);
  float g11 = sampleAt(lut.conductanceSiemens, x1, y1);

  KorenTriodePlateEval eval{};
  eval.currentAmps = lerp(lerp(i00, i10, tx), lerp(i01, i11, tx), ty);
  eval.conductanceSiemens = lerp(lerp(g00, g10, tx), lerp(g01, g11, tx), ty);
  return eval;
}

}  // namespace

TriodeOperatingPoint solveTriodeOperatingPoint(
    float plateSupplyVolts,
    float loadResistanceOhms,
    float biasVolts,
    float targetPlateVolts,
    float targetPlateCurrentAmps,
    float targetMutualConductanceSiemens,
    float mu) {
  KorenTriodeModel model = fitKorenTriodeModel(
      biasVolts, targetPlateVolts, targetPlateCurrentAmps,
      targetMutualConductanceSiemens, mu);
  float solvedPlateVolts = solveLoadLinePlateVoltage(
      plateSupplyVolts, loadResistanceOhms, targetPlateVolts,
      [&](float plateVolts) {
        return static_cast<float>(
            korenTriodePlateCurrent(biasVolts, plateVolts, model));
      });
  float solvedPlateCurrent = static_cast<float>(
      korenTriodePlateCurrent(biasVolts, solvedPlateVolts, model));
  float plateSlope = finiteDifferenceDerivative(
      [&](float plateVolts) {
        return static_cast<float>(
            korenTriodePlateCurrent(biasVolts, plateVolts, model));
      },
      solvedPlateVolts, 0.5f);
  float rpOhms = 1.0f / std::max(std::fabs(plateSlope), 1e-9f);
  return TriodeOperatingPoint{solvedPlateVolts, solvedPlateCurrent, rpOhms,
                              model};
}

PentodeOperatingPoint solvePentodeOperatingPoint(
    float plateSupplyVolts,
    float screenVolts,
    float loadResistanceOhms,
    float biasVolts,
    float targetPlateVolts,
    float targetPlateCurrentAmps,
    float targetMutualConductanceSiemens,
    float plateKneeVolts,
    float gridSoftnessVolts,
    float& fittedCutoffVolts) {
  auto solveOnce = [&](float referencePlateVolts,
                       float referencePlateCurrentAmps) {
    float safeReferencePlate = std::max(referencePlateVolts, 1.0f);
    float safeReferenceCurrent = std::max(referencePlateCurrentAmps, 1e-6f);
    fittedCutoffVolts = fitPentodeModelCutoffVolts(
        biasVolts, safeReferencePlate, screenVolts, safeReferenceCurrent,
        targetMutualConductanceSiemens, plateKneeVolts, gridSoftnessVolts);
    float solvedPlateVolts = solveLoadLinePlateVoltage(
        plateSupplyVolts, loadResistanceOhms, safeReferencePlate,
        [&](float plateVolts) {
          return deviceTubePlateCurrent(
              biasVolts, plateVolts, screenVolts, biasVolts, fittedCutoffVolts,
              safeReferencePlate, screenVolts, safeReferenceCurrent,
              targetMutualConductanceSiemens, plateKneeVolts,
              gridSoftnessVolts);
        });
    float solvedPlateCurrent = deviceTubePlateCurrent(
        biasVolts, solvedPlateVolts, screenVolts, biasVolts, fittedCutoffVolts,
        safeReferencePlate, screenVolts, safeReferenceCurrent,
        targetMutualConductanceSiemens, plateKneeVolts, gridSoftnessVolts);
    float plateSlope = finiteDifferenceDerivative(
        [&](float plateVolts) {
          return deviceTubePlateCurrent(
              biasVolts, std::max(plateVolts, 0.0f), screenVolts, biasVolts,
              fittedCutoffVolts, solvedPlateVolts, screenVolts,
              std::max(solvedPlateCurrent, 1e-6f),
              targetMutualConductanceSiemens, plateKneeVolts,
              gridSoftnessVolts);
        },
        solvedPlateVolts, 0.5f);
    float rpOhms = 1.0f / std::max(std::fabs(plateSlope), 1e-9f);
    return PentodeOperatingPoint{solvedPlateVolts, solvedPlateCurrent, rpOhms};
  };

  PentodeOperatingPoint solved =
      solveOnce(targetPlateVolts, targetPlateCurrentAmps);
  solved = solveOnce(solved.plateVolts, solved.plateCurrentAmps);
  return solved;
}

float solveCapCoupledGridVoltage(float sourceVolts,
                                 float previousCapVoltage,
                                 float dt,
                                 float couplingCapFarads,
                                 float sourceResistanceOhms,
                                 float gridLeakResistanceOhms,
                                 float biasVolts,
                                 float gridCurrentResistanceOhms) {
  float seriesTerm = requirePositiveFinite(sourceResistanceOhms) +
                     dt / requirePositiveFinite(couplingCapFarads);

  float leakConductance = 1.0f / requirePositiveFinite(gridLeakResistanceOhms);
  float gridCurrentConductance =
      1.0f / requirePositiveFinite(gridCurrentResistanceOhms);

  float gridVolts = sourceVolts - previousCapVoltage;

  for (int i = 0; i < 8; ++i) {
    float controlGridVolts = biasVolts + gridVolts;
    float positiveGridCurrent =
        (controlGridVolts > 0.0f) ? controlGridVolts * gridCurrentConductance
                                  : 0.0f;
    float positiveGridSlope =
        (controlGridVolts > 0.0f) ? gridCurrentConductance : 0.0f;

    float f = (sourceVolts - previousCapVoltage - gridVolts) / seriesTerm -
              leakConductance * gridVolts - positiveGridCurrent;

    float df = -1.0f / seriesTerm - leakConductance - positiveGridSlope;

    assert(std::fabs(df) > 1e-9f);
    gridVolts -= f / df;
  }

  return gridVolts;
}

namespace {

float deviceTubePlateCurrent(float gridVolts,
                             float plateVolts,
                             float screenVolts,
                             float biasVolts,
                             float cutoffVolts,
                             float quiescentPlateVolts,
                             float quiescentScreenVolts,
                             float quiescentPlateCurrentAmps,
                             float mutualConductanceSiemens,
                             float plateKneeVolts,
                             float gridSoftnessVolts) {
  float activationQ =
      softplusVolts(biasVolts - cutoffVolts, gridSoftnessVolts);
  float activation = softplusVolts(gridVolts - cutoffVolts, gridSoftnessVolts);
  activationQ = std::max(activationQ, 1e-5f);
  activation = std::max(activation, 0.0f);
  float exponent = clampf(mutualConductanceSiemens * activationQ /
                              requirePositiveFinite(quiescentPlateCurrentAmps),
                          1.05f, 10.0f);
  float kneeQ =
      1.0f - std::exp(-std::max(quiescentPlateVolts, 0.0f) /
                      requirePositiveFinite(plateKneeVolts));
  float knee =
      1.0f - std::exp(-std::max(plateVolts, 0.0f) /
                      requirePositiveFinite(plateKneeVolts));
  float screenScale = screenVolts / requirePositiveFinite(quiescentScreenVolts);
  float perveance =
      quiescentPlateCurrentAmps /
      (std::pow(activationQ, exponent) * std::max(kneeQ, 1e-6f));
  return perveance * std::pow(activation, exponent) *
         std::max(knee, 0.0f) * std::max(screenScale, 0.0f);
}

}  // namespace

float processResistorLoadedTubeStage(float gridVolts,
                                     float supplyScale,
                                     float plateSupplyVolts,
                                     float quiescentPlateVolts,
                                     float screenVolts,
                                     float biasVolts,
                                     float cutoffVolts,
                                     float quiescentPlateCurrentAmps,
                                     float mutualConductanceSiemens,
                                     float loadResistanceOhms,
                                     float plateKneeVolts,
                                     float gridSoftnessVolts,
                                     float& plateVoltageState) {
  float supply = requirePositiveFinite(plateSupplyVolts * supplyScale);
  float screen = requirePositiveFinite(screenVolts * supplyScale);
  float safeLoadResistance = requirePositiveFinite(loadResistanceOhms);
  float plateVoltage = (plateVoltageState > 0.0f)
                           ? plateVoltageState
                           : std::clamp(quiescentPlateVolts * supplyScale, 0.0f,
                                        supply);
  for (int i = 0; i < 4; ++i) {
    float current = deviceTubePlateCurrent(
        gridVolts, plateVoltage, screen, biasVolts, cutoffVolts,
        quiescentPlateVolts * supplyScale, screenVolts * supplyScale,
        quiescentPlateCurrentAmps, mutualConductanceSiemens, plateKneeVolts,
        gridSoftnessVolts);
    float targetPlate =
        std::clamp(supply - current * safeLoadResistance, 0.0f, supply);
    plateVoltage = 0.5f * (plateVoltage + targetPlate);
  }
  plateVoltageState = plateVoltage;
  float quiescentPlate =
      std::clamp(quiescentPlateVolts * supplyScale, 0.0f, supply);
  return quiescentPlate - plateVoltage;
}

float processResistorLoadedTriodeStage(float gridVolts,
                                       float supplyScale,
                                       float plateSupplyVolts,
                                       float quiescentPlateVolts,
                                       const KorenTriodeModel& model,
                                       const KorenTriodeLut* lut,
                                       float loadResistanceOhms,
                                       float& plateVoltageState) {
  float supply = requirePositiveFinite(plateSupplyVolts * supplyScale);
  float safeLoadResistance = requirePositiveFinite(loadResistanceOhms);
  float plateVoltage = (plateVoltageState > 0.0f)
                           ? plateVoltageState
                           : std::clamp(quiescentPlateVolts * supplyScale, 0.0f,
                                        supply);
  for (int i = 0; i < 4; ++i) {
    KorenTriodePlateEval eval =
        (lut != nullptr)
            ? evaluateKorenTriodePlateRuntime(gridVolts, plateVoltage, model,
                                              *lut)
            : evaluateKorenTriodePlate(gridVolts, plateVoltage, model);
    float current = static_cast<float>(eval.currentAmps);
    float targetPlate =
        std::clamp(supply - current * safeLoadResistance, 0.0f, supply);
    plateVoltage = 0.5f * (plateVoltage + targetPlate);
  }
  plateVoltageState = plateVoltage;
  float quiescentPlate =
      std::clamp(quiescentPlateVolts * supplyScale, 0.0f, supply);
  return quiescentPlate - plateVoltage;
}

FixedPlatePentodeEvaluator prepareFixedPlatePentodeEvaluator(
    float plateVolts,
    float screenVolts,
    float biasVolts,
    float cutoffVolts,
    float quiescentPlateVolts,
    float quiescentScreenVolts,
    float quiescentPlateCurrentAmps,
    float mutualConductanceSiemens,
    float plateKneeVolts,
    float gridSoftnessVolts) {
  float activationQ =
      std::max(softplusVolts(biasVolts - cutoffVolts, gridSoftnessVolts), 1e-5f);
  float exponent = clampf(mutualConductanceSiemens * activationQ /
                              requirePositiveFinite(quiescentPlateCurrentAmps),
                          1.05f, 10.0f);
  float knee =
      1.0f - std::exp(-std::max(plateVolts, 0.0f) /
                      requirePositiveFinite(plateKneeVolts));
  float kneeQ =
      1.0f - std::exp(-std::max(quiescentPlateVolts, 0.0f) /
                      requirePositiveFinite(plateKneeVolts));
  float screenScale = screenVolts / requirePositiveFinite(quiescentScreenVolts);
  float perveance =
      quiescentPlateCurrentAmps /
      (std::pow(activationQ, exponent) * std::max(kneeQ, 1e-6f));

  FixedPlatePentodeEvaluator out{};
  out.cutoffVolts = cutoffVolts;
  out.gridSoftnessVolts = gridSoftnessVolts;
  out.exponent = exponent;
  out.currentScale =
      perveance * std::max(knee, 0.0f) * std::max(screenScale, 0.0f);
  return out;
}

KorenTriodePlateEval evaluateKorenTriodePlateRuntime(
    double vgk,
    double vpk,
    const KorenTriodeModel& model,
    const KorenTriodeLut& lut) {
  if (lut.valid()) {
    return evaluateKorenTriodePlateLut(vgk, vpk, lut);
  }
  return evaluateKorenTriodePlate(vgk, vpk, model);
}

void configureRuntimeTriodeLut(KorenTriodeLut& lut,
                               const KorenTriodeModel& model,
                               float biasVolts,
                               float plateSupplyVolts,
                               float extraNegativeGridHeadroomVolts,
                               float positiveGridHeadroomVolts) {
  float vgkMin = std::max(-140.0f, biasVolts - extraNegativeGridHeadroomVolts);
  float vgkMax = std::max(10.0f, positiveGridHeadroomVolts);
  float vpkMax =
      std::max(plateSupplyVolts + 80.0f, 1.25f * plateSupplyVolts);
  configureKorenTriodeLut(lut, model, vgkMin, vgkMax, 257, 0.0f, vpkMax, 385);
}
