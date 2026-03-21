#include "radio.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <complex>
#include <limits>

using BlockStep = Radio1938::BlockStep;
using AllocateStep = Radio1938::AllocateStep;
using ConfigureStep = Radio1938::ConfigureStep;
using InitializeDependentStateStep = Radio1938::InitializeDependentStateStep;
using ProgramPathStep = Radio1938::ProgramPathStep;
using ResetStep = Radio1938::ResetStep;
using SampleControlStep = Radio1938::SampleControlStep;

static inline float clampf(float x, float a, float b) {
  return std::min(std::max(x, a), b);
}

static inline float wrapPhase(float phase) {
  while (phase >= kRadioTwoPi) phase -= kRadioTwoPi;
  while (phase < 0.0f) phase += kRadioTwoPi;
  return phase;
}

static inline float db2lin(float db) { return std::pow(10.0f, db / 20.0f); }

static inline float lin2db(float x) {
  return 20.0f * std::log10(std::max(x, kRadioLinDbFloor));
}

static inline float parallelResistance(float a, float b) {
  if (a <= 0.0f) return std::max(b, 0.0f);
  if (b <= 0.0f) return std::max(a, 0.0f);
  return (a * b) / std::max(a + b, 1e-9f);
}

static inline float diodeJunctionRectify(float vIn,
                                         float dropVolts,
                                         float junctionSlopeVolts) {
  float slope = std::max(junctionSlopeVolts, 1e-5f);
  float x = (vIn - dropVolts) / slope;
  if (x >= 20.0f) return vIn - dropVolts;
  if (x <= -20.0f) return 0.0f;
  return slope * std::log1p(std::exp(x));
}

static inline float softplusVolts(float x, float softnessVolts) {
  float slope = std::max(softnessVolts, 1e-4f);
  float y = x / slope;
  if (y >= 20.0f) return x;
  if (y <= -20.0f) return 0.0f;
  return slope * std::log1p(std::exp(y));
}

static inline double stableLog1pExp(double x) {
  if (x > 50.0) return x;
  if (x < -50.0) return std::exp(x);
  return std::log1p(std::exp(x));
}

static KorenTriodeModel makeKorenTriodeModel(double mu,
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

static double korenTriodePlateCurrent(double vgk,
                                      double vpk,
                                      const KorenTriodeModel& m);

static bool solveLinear3x3(float a[3][3], float b[3], float x[3]);

static float deviceTubePlateCurrent(float gridVolts,
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

template <typename Fn, typename T>
static T finiteDifferenceDerivative(Fn&& fn, T x, T delta) {
  T safeDelta = std::max(delta, static_cast<T>(1e-4));
  T high = fn(x + safeDelta);
  T low = fn(x - safeDelta);
  return (high - low) / (static_cast<T>(2.0) * safeDelta);
}

struct KorenTriodeMetrics {
  double currentAmps = 0.0;
  double gmSiemens = 0.0;
  double gpSiemens = 0.0;
};

static KorenTriodeMetrics evaluateKorenTriodeMetrics(
    double vgkQ,
    double vpkQ,
    const KorenTriodeModel& m) {
  KorenTriodeMetrics metrics{};
  metrics.currentAmps = korenTriodePlateCurrent(vgkQ, vpkQ, m);
  metrics.gmSiemens = finiteDifferenceDerivative(
      [&](double vgk) { return korenTriodePlateCurrent(vgk, vpkQ, m); }, vgkQ,
      0.01);
  metrics.gpSiemens = finiteDifferenceDerivative(
      [&](double vpk) { return korenTriodePlateCurrent(vgkQ, vpk, m); }, vpkQ,
      0.25);
  return metrics;
}

static KorenTriodeModel fitKorenTriodeModel(double vgkQ,
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
  double kvbInit = std::max(vpkQ * vpkQ, 1.0);
  KorenTriodeModel initShape = makeKorenTriodeModel(mu, 1.0, kpInit, kvbInit);
  double initCurrent = korenTriodePlateCurrent(vgkQ, vpkQ, initShape);
  double kg1Init =
      (initCurrent > 0.0)
          ? (initCurrent / iqTarget)
          : (std::pow(std::max(vpkQ, 1.0), initShape.ex) / iqTarget);

  double u[3] = {std::log(kg1Init), std::log(kpInit), std::log(kvbInit)};
  double bestU[3] = {u[0], u[1], u[2]};
  double bestNorm = std::numeric_limits<double>::infinity();

  auto evalResiduals = [&](const double params[3], double residuals[3]) {
    KorenTriodeModel model = makeKorenTriodeModel(
        mu, std::exp(params[0]), std::exp(params[1]), std::exp(params[2]));
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

    u[0] = candidateBestU[0];
    u[1] = candidateBestU[1];
    u[2] = candidateBestU[2];

    double maxDelta =
        std::max(std::fabs(delta[0]),
                 std::max(std::fabs(delta[1]), std::fabs(delta[2])));
    if (maxDelta < 1e-6) break;
  }

  return makeKorenTriodeModel(mu, std::exp(bestU[0]), std::exp(bestU[1]),
                              std::exp(bestU[2]));
}

static float fitPentodeModelCutoffVolts(float biasVolts,
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
static float solveLoadLinePlateVoltage(float plateSupplyVolts,
                                       float loadResistanceOhms,
                                       float initialGuessVolts,
                                       PlateCurrentFn&& plateCurrentForPlate) {
  float safeSupply = std::max(plateSupplyVolts, 1.0f);
  if (loadResistanceOhms <= 0.0f) {
    return clampf(initialGuessVolts, 0.0f, safeSupply);
  }

  auto residual = [&](float plateVolts) {
    float safePlate = clampf(plateVolts, 0.0f, safeSupply);
    return safeSupply -
           plateCurrentForPlate(safePlate) * std::max(loadResistanceOhms, 1.0f) -
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
    float targetPlate =
        safeSupply - plateCurrentForPlate(plateVolts) *
                         std::max(loadResistanceOhms, 1.0f);
    targetPlate = clampf(targetPlate, 0.0f, safeSupply);
    plateVolts = 0.5f * (plateVolts + targetPlate);
  }
  return plateVolts;
}

static TriodeOperatingPoint solveTriodeOperatingPoint(
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

static PentodeOperatingPoint solvePentodeOperatingPoint(
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
              fittedCutoffVolts, solvedPlateVolts,
              std::max(solvedPlateCurrent, 1e-6f), screenVolts,
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

static float tubeGridBranchCurrent(float acGridVolts,
                                   float biasVolts,
                                   float gridLeakResistanceOhms,
                                   float gridCurrentResistanceOhms) {
  float leakCurrent = acGridVolts / std::max(gridLeakResistanceOhms, 1.0f);

  float controlGridVolts = biasVolts + acGridVolts;
  float positiveGridCurrent =
      (controlGridVolts > 0.0f)
          ? (controlGridVolts /
             std::max(gridCurrentResistanceOhms, 1.0f))
          : 0.0f;

  return leakCurrent + positiveGridCurrent;
}

static float tubeGridBranchSlope(float acGridVolts,
                                 float biasVolts,
                                 float gridLeakResistanceOhms,
                                 float gridCurrentResistanceOhms) {
  float slope = 1.0f / std::max(gridLeakResistanceOhms, 1.0f);
  if (biasVolts + acGridVolts > 0.0f) {
    slope += 1.0f / std::max(gridCurrentResistanceOhms, 1.0f);
  }
  return slope;
}

static float solveCapCoupledGridVoltage(float sourceVolts,
                                        float previousCapVoltage,
                                        float dt,
                                        float couplingCapFarads,
                                        float sourceResistanceOhms,
                                        float gridLeakResistanceOhms,
                                        float biasVolts,
                                        float gridCurrentResistanceOhms) {
  float seriesTerm = std::max(sourceResistanceOhms, 1.0f) +
                     dt / std::max(couplingCapFarads, 1e-12f);

  float leakConductance = 1.0f / std::max(gridLeakResistanceOhms, 1.0f);
  float gridCurrentConductance =
      1.0f / std::max(gridCurrentResistanceOhms, 1.0f);

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

    gridVolts -= f / std::max(std::fabs(df), 1e-9f);
  }

  return gridVolts;
}

static float scalePhysicalSpeakerVoltsToDigital(const Radio1938& radio,
                                                float speakerVolts) {
  if (!radio.graph.isEnabled(StageId::Power)) return speakerVolts;
  return speakerVolts /
         std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f);
}

static float deviceTubePlateCurrent(float gridVolts,
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
  float activation =
      softplusVolts(gridVolts - cutoffVolts, gridSoftnessVolts);
  activationQ = std::max(activationQ, 1e-5f);
  activation = std::max(activation, 0.0f);
  float exponent = clampf(mutualConductanceSiemens * activationQ /
                              std::max(quiescentPlateCurrentAmps, 1e-9f),
                          1.05f, 10.0f);
  float kneeQ =
      1.0f - std::exp(-std::max(quiescentPlateVolts, 0.0f) /
                      std::max(plateKneeVolts, 1e-3f));
  float knee =
      1.0f - std::exp(-std::max(plateVolts, 0.0f) /
                      std::max(plateKneeVolts, 1e-3f));
  float screenScale =
      std::max(screenVolts, 1e-3f) / std::max(quiescentScreenVolts, 1e-3f);
  float perveance =
      quiescentPlateCurrentAmps /
      (std::pow(activationQ, exponent) * std::max(kneeQ, 1e-6f));
  return perveance * std::pow(activation, exponent) *
         std::max(knee, 0.0f) * std::max(screenScale, 0.0f);
}

static float processResistorLoadedTubeStage(float gridVolts,
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
  float supply = std::max(plateSupplyVolts * supplyScale, 1.0f);
  float screen = std::max(screenVolts * supplyScale, 1.0f);
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
        std::clamp(supply - current * std::max(loadResistanceOhms, 1.0f), 0.0f,
                   supply);
    plateVoltage = 0.5f * (plateVoltage + targetPlate);
  }
  plateVoltageState = plateVoltage;
  float quiescentPlate = std::clamp(quiescentPlateVolts * supplyScale, 0.0f, supply);
  return quiescentPlate - plateVoltage;
}

static float processResistorLoadedTriodeStage(
    float gridVolts,
    float supplyScale,
    float plateSupplyVolts,
    float quiescentPlateVolts,
    const KorenTriodeModel& model,
    float loadResistanceOhms,
    float& plateVoltageState) {
  float supply = std::max(plateSupplyVolts * supplyScale, 1.0f);
  float plateVoltage = (plateVoltageState > 0.0f)
                           ? plateVoltageState
                           : std::clamp(quiescentPlateVolts * supplyScale, 0.0f,
                                        supply);
  for (int i = 0; i < 4; ++i) {
    float current =
        static_cast<float>(korenTriodePlateCurrent(gridVolts, plateVoltage, model));
    float targetPlate =
        std::clamp(supply - current * std::max(loadResistanceOhms, 1.0f), 0.0f,
                   supply);
    plateVoltage = 0.5f * (plateVoltage + targetPlate);
  }
  plateVoltageState = plateVoltage;
  float quiescentPlate = std::clamp(quiescentPlateVolts * supplyScale, 0.0f, supply);
  return quiescentPlate - plateVoltage;
}

static float processConverterTubeStage(float baseGridVolts,
                                       float rfGridVolts,
                                       float oscillatorBiasVolts,
                                       float oscillatorGridVolts,
                                       float supplyScale,
                                       float quiescentPlateVolts,
                                       float screenVolts,
                                       float biasVolts,
                                       float cutoffVolts,
                                       float quiescentPlateCurrentAmps,
                                       float mutualConductanceSiemens,
                                       float acLoadResistanceOhms,
                                       float plateKneeVolts,
                                       float gridSoftnessVolts) {
  float plateVoltage = std::max(quiescentPlateVolts * supplyScale, 1.0f);
  float screen = std::max(screenVolts * supplyScale, 1.0f);
  auto plateCurrentForGrid = [&](float gridVolts) {
    return deviceTubePlateCurrent(
        gridVolts, plateVoltage, screen, biasVolts, cutoffVolts,
        quiescentPlateVolts * supplyScale, screenVolts * supplyScale,
        quiescentPlateCurrentAmps, mutualConductanceSiemens, plateKneeVolts,
        gridSoftnessVolts);
  };
  float mixedBaseGridVolts = baseGridVolts + oscillatorBiasVolts;
  float idleCurrent = plateCurrentForGrid(mixedBaseGridVolts);
  float rfOnlyCurrent =
      plateCurrentForGrid(mixedBaseGridVolts + rfGridVolts);
  float loOnlyCurrent =
      plateCurrentForGrid(mixedBaseGridVolts + oscillatorGridVolts);
  float mixedCurrent =
      plateCurrentForGrid(mixedBaseGridVolts + rfGridVolts +
                          oscillatorGridVolts);
  float conversionCurrent =
      mixedCurrent - rfOnlyCurrent - loOnlyCurrent + idleCurrent;
  return conversionCurrent * std::max(acLoadResistanceOhms, 1.0f);
}

static bool solveLinear3x3(float a[3][3], float b[3], float x[3]) {
  for (int pivot = 0; pivot < 3; ++pivot) {
    int pivotRow = pivot;
    float pivotAbs = std::fabs(a[pivot][pivot]);
    for (int row = pivot + 1; row < 3; ++row) {
      float candidateAbs = std::fabs(a[row][pivot]);
      if (candidateAbs > pivotAbs) {
        pivotAbs = candidateAbs;
        pivotRow = row;
      }
    }
    if (pivotAbs < 1e-12f) return false;
    if (pivotRow != pivot) {
      for (int col = pivot; col < 3; ++col) std::swap(a[pivot][col], a[pivotRow][col]);
      std::swap(b[pivot], b[pivotRow]);
    }
    float invPivot = 1.0f / a[pivot][pivot];
    for (int row = pivot + 1; row < 3; ++row) {
      float scale = a[row][pivot] * invPivot;
      if (std::fabs(scale) < 1e-12f) continue;
      for (int col = pivot; col < 3; ++col) a[row][col] -= scale * a[pivot][col];
      b[row] -= scale * b[pivot];
    }
  }
  for (int row = 2; row >= 0; --row) {
    float sum = b[row];
    for (int col = row + 1; col < 3; ++col) sum -= a[row][col] * x[col];
    if (std::fabs(a[row][row]) < 1e-12f) return false;
    x[row] = sum / a[row][row];
  }
  return true;
}

static bool solveLinear4x4(float a[4][4], float b[4], float x[4]) {
  for (int pivot = 0; pivot < 4; ++pivot) {
    int pivotRow = pivot;
    float pivotAbs = std::fabs(a[pivot][pivot]);
    for (int row = pivot + 1; row < 4; ++row) {
      float candidateAbs = std::fabs(a[row][pivot]);
      if (candidateAbs > pivotAbs) {
        pivotAbs = candidateAbs;
        pivotRow = row;
      }
    }
    if (pivotAbs < 1e-12f) return false;
    if (pivotRow != pivot) {
      for (int col = pivot; col < 4; ++col) {
        std::swap(a[pivot][col], a[pivotRow][col]);
      }
      std::swap(b[pivot], b[pivotRow]);
    }
    float invPivot = 1.0f / a[pivot][pivot];
    for (int row = pivot + 1; row < 4; ++row) {
      float scale = a[row][pivot] * invPivot;
      if (std::fabs(scale) < 1e-12f) continue;
      for (int col = pivot; col < 4; ++col) {
        a[row][col] -= scale * a[pivot][col];
      }
      b[row] -= scale * b[pivot];
    }
  }
  for (int row = 3; row >= 0; --row) {
    float sum = b[row];
    for (int col = row + 1; col < 4; ++col) sum -= a[row][col] * x[col];
    if (std::fabs(a[row][row]) < 1e-12f) return false;
    x[row] = sum / a[row][row];
  }
  return true;
}

static bool solveLinear5x5(float a[5][5], float b[5], float x[5]) {
  for (int pivot = 0; pivot < 5; ++pivot) {
    int pivotRow = pivot;
    float pivotAbs = std::fabs(a[pivot][pivot]);
    for (int row = pivot + 1; row < 5; ++row) {
      float candidateAbs = std::fabs(a[row][pivot]);
      if (candidateAbs > pivotAbs) {
        pivotAbs = candidateAbs;
        pivotRow = row;
      }
    }
    if (pivotAbs < 1e-12f) return false;
    if (pivotRow != pivot) {
      for (int col = pivot; col < 5; ++col) {
        std::swap(a[pivot][col], a[pivotRow][col]);
      }
      std::swap(b[pivot], b[pivotRow]);
    }
    float invPivot = 1.0f / a[pivot][pivot];
    for (int row = pivot + 1; row < 5; ++row) {
      float scale = a[row][pivot] * invPivot;
      if (std::fabs(scale) < 1e-12f) continue;
      for (int col = pivot; col < 5; ++col) {
        a[row][col] -= scale * a[pivot][col];
      }
      b[row] -= scale * b[pivot];
    }
  }
  for (int row = 4; row >= 0; --row) {
    float sum = b[row];
    for (int col = row + 1; col < 5; ++col) {
      sum -= a[row][col] * x[col];
    }
    if (std::fabs(a[row][row]) < 1e-12f) return false;
    x[row] = sum / a[row][row];
  }
  return true;
}

static double korenTriodePlateCurrent(double vgk,
                                      double vpk,
                                      const KorenTriodeModel& m) {
  if (vpk <= 0.0) {
    return 0.0;
  }

  double term = (1.0 / m.mu) + vgk / std::sqrt(m.kvb + vpk * vpk);
  double e1 = (vpk / m.kp) * stableLog1pExp(m.kp * term);

  if (e1 <= 0.0) {
    return 0.0;
  }

  return std::pow(e1, m.ex) / m.kg1;
}

static inline uint32_t hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static inline float seededSignedUnit(uint32_t seed, uint32_t salt) {
  uint32_t h = hash32(seed ^ salt);
  return 2.0f * (static_cast<float>(h) * kRadioHashUnitInv) - 1.0f;
}

static inline float applySeededDrift(float base,
                                     float relativeDepth,
                                     uint32_t seed,
                                     uint32_t salt) {
  return base * (1.0f + relativeDepth * seededSignedUnit(seed, salt));
}

template <typename Nonlinear>
static inline float processOversampled2x(float x,
                                         float& prev,
                                         Biquad& lp1,
                                         Biquad& lp2,
                                         Nonlinear&& nonlinear) {
  float mid = 0.5f * (prev + x);
  float y0 = nonlinear(mid);
  float y1 = nonlinear(x);
  y0 = lp1.process(y0);
  y0 = lp2.process(y0);
  y1 = lp1.process(y1);
  y1 = lp2.process(y1);
  prev = x;
  return y1;
}

float Biquad::process(float x) {
  float y = b0 * x + z1;
  z1 = b1 * x - a1 * y + z2;
  z2 = b2 * x - a2 * y;
  return y;
}

void Biquad::reset() {
  z1 = 0.0f;
  z2 = 0.0f;
}

void Biquad::setLowpass(float sampleRate, float freq, float q) {
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha;
  b0 = (1.0f - cosw) / 2.0f / a0;
  b1 = (1.0f - cosw) / a0;
  b2 = (1.0f - cosw) / 2.0f / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha) / a0;
}

void Biquad::setHighpass(float sampleRate, float freq, float q) {
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha;
  b0 = (1.0f + cosw) / 2.0f / a0;
  b1 = -(1.0f + cosw) / a0;
  b2 = (1.0f + cosw) / 2.0f / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha) / a0;
}

void Biquad::setBandpass(float sampleRate, float freq, float q) {
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha;
  b0 = alpha / a0;
  b1 = 0.0f;
  b2 = -alpha / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha) / a0;
}

void Biquad::setPeaking(float sampleRate, float freq, float q, float gainDb) {
  float a = std::pow(10.0f, gainDb / 40.0f);
  float w0 = kRadioTwoPi * freq / sampleRate;
  float cosw = std::cos(w0);
  float sinw = std::sin(w0);
  float alpha = sinw / (2.0f * q);
  float a0 = 1.0f + alpha / a;
  b0 = (1.0f + alpha * a) / a0;
  b1 = -2.0f * cosw / a0;
  b2 = (1.0f - alpha * a) / a0;
  a1 = -2.0f * cosw / a0;
  a2 = (1.0f - alpha / a) / a0;
}

void SeriesRlcBandpass::configure(float newFs,
                                  float newInductanceHenries,
                                  float newCapacitanceFarads,
                                  float newSeriesResistanceOhms,
                                  float newOutputResistanceOhms,
                                  int newIntegrationSubsteps) {
  fs = newFs;
  inductanceHenries = newInductanceHenries;
  capacitanceFarads = newCapacitanceFarads;
  seriesResistanceOhms = newSeriesResistanceOhms;
  outputResistanceOhms = newOutputResistanceOhms;
  integrationSubsteps = newIntegrationSubsteps;
  reset();
}

void SeriesRlcBandpass::reset() {
  inductorCurrent = 0.0f;
  capacitorVoltage = 0.0f;
}

float SeriesRlcBandpass::process(float vin) {
  float dt = 1.0f / (fs * static_cast<float>(integrationSubsteps));
  for (int step = 0; step < integrationSubsteps; ++step) {
    float di =
        (vin - seriesResistanceOhms * inductorCurrent - capacitorVoltage) /
        inductanceHenries;
    inductorCurrent += dt * di;
    capacitorVoltage += dt * (inductorCurrent / capacitanceFarads);
    if (!std::isfinite(inductorCurrent) || !std::isfinite(capacitorVoltage)) {
      reset();
      return 0.0f;
    }
  }
  return inductorCurrent * outputResistanceOhms;
}

void CoupledTunedTransformer::configure(float newFs,
                                        float newPrimaryInductanceHenries,
                                        float newPrimaryCapacitanceFarads,
                                        float newPrimaryResistanceOhms,
                                        float newSecondaryInductanceHenries,
                                        float newSecondaryCapacitanceFarads,
                                        float newSecondaryResistanceOhms,
                                        float newCouplingCoeff,
                                        float newOutputResistanceOhms,
                                        int newIntegrationSubsteps) {
  fs = newFs;
  primaryInductanceHenries = newPrimaryInductanceHenries;
  primaryCapacitanceFarads = newPrimaryCapacitanceFarads;
  primaryResistanceOhms = newPrimaryResistanceOhms;
  secondaryInductanceHenries = newSecondaryInductanceHenries;
  secondaryCapacitanceFarads = newSecondaryCapacitanceFarads;
  secondaryResistanceOhms = newSecondaryResistanceOhms;
  couplingCoeff = newCouplingCoeff;
  outputResistanceOhms = newOutputResistanceOhms;
  integrationSubsteps = newIntegrationSubsteps;
  reset();
}

void CoupledTunedTransformer::reset() {
  primaryCurrent = 0.0f;
  primaryCapVoltage = 0.0f;
  secondaryCurrent = 0.0f;
  secondaryCapVoltage = 0.0f;
}

float CoupledTunedTransformer::process(float vin) {
  float mutualInductance =
      couplingCoeff *
      std::sqrt(primaryInductanceHenries * secondaryInductanceHenries);
  float determinant = std::max(
      primaryInductanceHenries * secondaryInductanceHenries -
          mutualInductance * mutualInductance,
      1e-18f);
  float dt = 1.0f / (fs * static_cast<float>(integrationSubsteps));

  for (int step = 0; step < integrationSubsteps; ++step) {
    float primaryDrive =
        vin - primaryResistanceOhms * primaryCurrent - primaryCapVoltage;
    float secondaryDrive =
        -secondaryResistanceOhms * secondaryCurrent - secondaryCapVoltage;
    float dPrimary =
        (secondaryInductanceHenries * primaryDrive -
         mutualInductance * secondaryDrive) /
        determinant;
    float dSecondary =
        (primaryInductanceHenries * secondaryDrive -
         mutualInductance * primaryDrive) /
        determinant;
    primaryCurrent += dt * dPrimary;
    secondaryCurrent += dt * dSecondary;
    primaryCapVoltage += dt * (primaryCurrent / primaryCapacitanceFarads);
    secondaryCapVoltage += dt * (secondaryCurrent / secondaryCapacitanceFarads);
    if (!std::isfinite(primaryCurrent) || !std::isfinite(primaryCapVoltage) ||
        !std::isfinite(secondaryCurrent) ||
        !std::isfinite(secondaryCapVoltage)) {
      reset();
      return 0.0f;
    }
  }

  return secondaryCurrent * outputResistanceOhms;
}

static CurrentDrivenTransformerSample projectNoShuntCap(
    const CurrentDrivenTransformer& transformer,
    float primaryDriveCurrentAmps,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms) {
  float dt =
      1.0f / (transformer.fs * static_cast<float>(transformer.integrationSubsteps));
  float safeTurns = std::max(transformer.turnsRatioPrimaryToSecondary, 1e-4f);
  float Rp = transformer.primaryResistanceOhms;
  float Rs = transformer.secondaryResistanceOhms;
  float Lp = transformer.primaryLeakageInductanceHenries +
             transformer.magnetizingInductanceHenries;
  float Ls = transformer.secondaryLeakageInductanceHenries +
             transformer.magnetizingInductanceHenries /
                 (safeTurns * safeTurns);
  float M = transformer.magnetizingInductanceHenries / safeTurns;
  float coreLossConductance =
      (transformer.primaryCoreLossResistanceOhms > 0.0f)
          ? 1.0f / std::max(transformer.primaryCoreLossResistanceOhms, 1e-9f)
          : 0.0f;
  float primaryLoadConductance =
      (primaryLoadResistanceOhms > 0.0f &&
       std::isfinite(primaryLoadResistanceOhms))
          ? 1.0f / std::max(primaryLoadResistanceOhms, 1e-9f)
          : 0.0f;
  float Gp = coreLossConductance + primaryLoadConductance;
  float Gs = (secondaryLoadResistanceOhms > 0.0f &&
              std::isfinite(secondaryLoadResistanceOhms))
                 ? 1.0f / std::max(secondaryLoadResistanceOhms, 1e-9f)
                 : 0.0f;
  float projectedPrimaryCurrent = transformer.primaryCurrent;
  float projectedSecondaryCurrent = transformer.secondaryCurrent;
  float projectedPrimaryVoltage = transformer.primaryVoltage;
  float projectedSecondaryVoltage = transformer.secondaryVoltage;

  for (int step = 0; step < transformer.integrationSubsteps; ++step) {
    float ipPrev = projectedPrimaryCurrent;
    float isPrev = projectedSecondaryCurrent;
    float a[4][4] = {
        {Rp + Lp / dt, M / dt, -1.0f, 0.0f},
        {M / dt, Rs + Ls / dt, 0.0f, -1.0f},
        {1.0f, 0.0f, Gp, 0.0f},
        {0.0f, 1.0f, 0.0f, Gs},
    };
    float b[4] = {
        (Lp / dt) * ipPrev + (M / dt) * isPrev,
        (M / dt) * ipPrev + (Ls / dt) * isPrev,
        primaryDriveCurrentAmps,
        0.0f,
    };
    float x[4] = {projectedPrimaryCurrent, projectedSecondaryCurrent,
                  projectedPrimaryVoltage, projectedSecondaryVoltage};
    if (!solveLinear4x4(a, b, x)) return CurrentDrivenTransformerSample{};
    projectedPrimaryCurrent = x[0];
    projectedSecondaryCurrent = x[1];
    projectedPrimaryVoltage = x[2];
    projectedSecondaryVoltage = x[3];
    if (!std::isfinite(projectedPrimaryCurrent) ||
        !std::isfinite(projectedSecondaryCurrent) ||
        !std::isfinite(projectedPrimaryVoltage) ||
        !std::isfinite(projectedSecondaryVoltage)) {
      return CurrentDrivenTransformerSample{};
    }
  }

  return CurrentDrivenTransformerSample{
      projectedPrimaryVoltage, projectedSecondaryVoltage,
      projectedPrimaryCurrent, projectedSecondaryCurrent};
}

static float interstageDifferentialGridCurrent(
    float vs,
    float biasVolts,
    float gridLeakResistanceOhms,
    float gridCurrentResistanceOhms) {
  float va = 0.5f * vs;
  float vb = -0.5f * vs;
  float ia = tubeGridBranchCurrent(va, biasVolts, gridLeakResistanceOhms,
                                   gridCurrentResistanceOhms);
  float ib = tubeGridBranchCurrent(vb, biasVolts, gridLeakResistanceOhms,
                                   gridCurrentResistanceOhms);
  return 0.5f * (ia - ib);
}

static float interstageDifferentialGridCurrentSlope(
    float vs,
    float biasVolts,
    float gridLeakResistanceOhms,
    float gridCurrentResistanceOhms) {
  float va = 0.5f * vs;
  float vb = -0.5f * vs;
  return 0.25f *
         (tubeGridBranchSlope(va, biasVolts, gridLeakResistanceOhms,
                              gridCurrentResistanceOhms) +
          tubeGridBranchSlope(vb, biasVolts, gridLeakResistanceOhms,
                              gridCurrentResistanceOhms));
}

struct DriverInterstageSolveResult {
  CurrentDrivenTransformerSample transformer{};
  float driverPlateCurrentAbs = 0.0f;
};

static DriverInterstageSolveResult solveDriverInterstageNoCap(
    const CurrentDrivenTransformer& transformer,
    float controlGridVolts,
    float driverPlateQuiescent,
    float driverQuiescentCurrent,
    const KorenTriodeModel& driverTriodeModel,
    float primaryLoadResistanceOhms,
    float outputTubeBiasVolts,
    float outputGridLeakResistanceOhms,
    float outputGridCurrentResistanceOhms) {
  assert(transformer.primaryShuntCapFarads <= 0.0f &&
         transformer.secondaryShuntCapFarads <= 0.0f);
  float dt =
      1.0f / (transformer.fs * static_cast<float>(transformer.integrationSubsteps));

  float n = std::max(transformer.turnsRatioPrimaryToSecondary, 1e-4f);
  float Rp = transformer.primaryResistanceOhms;
  float Rs = transformer.secondaryResistanceOhms;
  float Lp = transformer.primaryLeakageInductanceHenries +
             transformer.magnetizingInductanceHenries;
  float Ls = transformer.secondaryLeakageInductanceHenries +
             transformer.magnetizingInductanceHenries / (n * n);
  float M = transformer.magnetizingInductanceHenries / n;

  float coreLossConductance =
      (transformer.primaryCoreLossResistanceOhms > 0.0f)
          ? 1.0f / std::max(transformer.primaryCoreLossResistanceOhms, 1e-9f)
          : 0.0f;

  float primaryLoadConductance =
      (primaryLoadResistanceOhms > 0.0f &&
       std::isfinite(primaryLoadResistanceOhms))
          ? 1.0f / std::max(primaryLoadResistanceOhms, 1e-9f)
          : 0.0f;

  float Gp = coreLossConductance + primaryLoadConductance;

  float ip = transformer.primaryCurrent;
  float is = transformer.secondaryCurrent;
  float vp = transformer.primaryVoltage;
  float vs = transformer.secondaryVoltage;
  float idrive = ip + Gp * vp;

  for (int step = 0; step < transformer.integrationSubsteps; ++step) {
    float ipPrev = ip;
    float isPrev = is;

    for (int iter = 0; iter < 12; ++iter) {
      float driverPlateVolts = driverPlateQuiescent - vp;

      float driverPlateCurrentAbs = static_cast<float>(korenTriodePlateCurrent(
          controlGridVolts, driverPlateVolts, driverTriodeModel));

      float idiff = interstageDifferentialGridCurrent(
          vs, outputTubeBiasVolts, outputGridLeakResistanceOhms,
          outputGridCurrentResistanceOhms);

      float dIdiff_dVs = interstageDifferentialGridCurrentSlope(
          vs, outputTubeBiasVolts, outputGridLeakResistanceOhms,
          outputGridCurrentResistanceOhms);

      auto driverPlateCurrentFromPlateVolts = [&](float plateVolts) {
        return static_cast<float>(
            korenTriodePlateCurrent(controlGridVolts, plateVolts,
                                    driverTriodeModel));
      };

      float dIdriver_dVp = finiteDifferenceDerivative(
          driverPlateCurrentFromPlateVolts, driverPlateQuiescent - vp, 0.25f);

      float f[5] = {
          (Rp + Lp / dt) * ip + (M / dt) * is - vp -
              ((Lp / dt) * ipPrev + (M / dt) * isPrev),

          (M / dt) * ip + (Rs + Ls / dt) * is - vs -
              ((M / dt) * ipPrev + (Ls / dt) * isPrev),

          ip + Gp * vp - idrive,

          is + idiff,

          idrive - (driverPlateCurrentAbs - driverQuiescentCurrent),
      };

      float j[5][5] = {
          {0.0f, Rp + Lp / dt, M / dt, -1.0f, 0.0f},
          {0.0f, M / dt, Rs + Ls / dt, 0.0f, -1.0f},
          {-1.0f, 1.0f, 0.0f, Gp, 0.0f},
          {0.0f, 0.0f, 1.0f, 0.0f, dIdiff_dVs},
          {1.0f, 0.0f, 0.0f, dIdriver_dVp, 0.0f},
      };

      float rhs[5] = {-f[0], -f[1], -f[2], -f[3], -f[4]};
      float delta[5] = {};
      if (!solveLinear5x5(j, rhs, delta)) {
        return {};
      }

      float lambda = 1.0f;
      float bestNorm = std::fabs(f[0]) + std::fabs(f[1]) + std::fabs(f[2]) +
                       std::fabs(f[3]) + std::fabs(f[4]);

      float bestIdrive = idrive;
      float bestIp = ip;
      float bestIs = is;
      float bestVp = vp;
      float bestVs = vs;

      for (int ls = 0; ls < 8; ++ls) {
        float candIdrive = idrive + lambda * delta[0];
        float candIp = ip + lambda * delta[1];
        float candIs = is + lambda * delta[2];
        float candVp = vp + lambda * delta[3];
        float candVs = vs + lambda * delta[4];

        if (!std::isfinite(candIdrive) || !std::isfinite(candIp) ||
            !std::isfinite(candIs) || !std::isfinite(candVp) ||
            !std::isfinite(candVs)) {
          lambda *= 0.5f;
          continue;
        }

        float candPlateVolts = driverPlateQuiescent - candVp;
        float candDriverAbs = static_cast<float>(korenTriodePlateCurrent(
            controlGridVolts, candPlateVolts, driverTriodeModel));

        float candIdiff = interstageDifferentialGridCurrent(
            candVs, outputTubeBiasVolts, outputGridLeakResistanceOhms,
            outputGridCurrentResistanceOhms);

        float fCand[5] = {
            (Rp + Lp / dt) * candIp + (M / dt) * candIs - candVp -
                ((Lp / dt) * ipPrev + (M / dt) * isPrev),

            (M / dt) * candIp + (Rs + Ls / dt) * candIs - candVs -
                ((M / dt) * ipPrev + (Ls / dt) * isPrev),

            candIp + Gp * candVp - candIdrive,

            candIs + candIdiff,

            candIdrive - (candDriverAbs - driverQuiescentCurrent),
        };

        float candNorm = std::fabs(fCand[0]) + std::fabs(fCand[1]) +
                         std::fabs(fCand[2]) + std::fabs(fCand[3]) +
                         std::fabs(fCand[4]);

        if (candNorm < bestNorm) {
          bestNorm = candNorm;
          bestIdrive = candIdrive;
          bestIp = candIp;
          bestIs = candIs;
          bestVp = candVp;
          bestVs = candVs;
        }

        lambda *= 0.5f;
      }

      idrive = bestIdrive;
      ip = bestIp;
      is = bestIs;
      vp = bestVp;
      vs = bestVs;

      float maxDelta = std::max(
          std::fabs(delta[0]),
          std::max(std::fabs(delta[1]),
                   std::max(std::fabs(delta[2]),
                            std::max(std::fabs(delta[3]),
                                     std::fabs(delta[4])))));

      if (maxDelta < 1e-6f) {
        break;
      }
    }
  }

  float finalDriverPlateVolts = driverPlateQuiescent - vp;
  float finalDriverPlateCurrentAbs = static_cast<float>(
      korenTriodePlateCurrent(controlGridVolts, finalDriverPlateVolts,
                              driverTriodeModel));

  return {{vp, vs, ip, is}, finalDriverPlateCurrentAbs};
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

  float dt = 1.0f / (fs * static_cast<float>(integrationSubsteps));
  float safeTurns = std::max(turnsRatioPrimaryToSecondary, 1e-4f);
  float primaryInductance =
      primaryLeakageInductanceHenries + magnetizingInductanceHenries;
  float secondaryInductance =
      secondaryLeakageInductanceHenries +
      magnetizingInductanceHenries / (safeTurns * safeTurns);
  float mutualInductance = magnetizingInductanceHenries / safeTurns;
  float primaryCap = std::max(primaryShuntCapFarads, 1e-15f);
  float secondaryCap = std::max(secondaryShuntCapFarads, 1e-15f);
  float primaryCoreConductance =
      (primaryCoreLossResistanceOhms > 0.0f)
          ? 1.0f / std::max(primaryCoreLossResistanceOhms, 1e-9f)
          : 0.0f;
  if (primaryLoadResistanceOhms > 0.0f &&
      std::isfinite(primaryLoadResistanceOhms)) {
    primaryCoreConductance +=
        1.0f / std::max(primaryLoadResistanceOhms, 1e-9f);
  }
  float secondaryLoadConductance =
      (secondaryLoadResistanceOhms > 0.0f &&
       std::isfinite(secondaryLoadResistanceOhms))
          ? 1.0f / std::max(secondaryLoadResistanceOhms, 1e-9f)
          : 0.0f;
  float determinant = std::max(
      primaryInductance * secondaryInductance - mutualInductance * mutualInductance,
      1e-18f);
  float a11 = secondaryInductance / determinant;
  float a12 = -mutualInductance / determinant;
  float a21 = -mutualInductance / determinant;
  float a22 = primaryInductance / determinant;
  float projectedPrimaryCurrent = primaryCurrent;
  float projectedSecondaryCurrent = secondaryCurrent;
  float projectedPrimaryVoltage = primaryVoltage;
  float projectedSecondaryVoltage = secondaryVoltage;

  for (int step = 0; step < integrationSubsteps; ++step) {
    float system[4][4] = {
        {1.0f + 0.5f * dt * a11 * primaryResistanceOhms,
         0.5f * dt * a12 * secondaryResistanceOhms,
         -0.5f * dt * a11, -0.5f * dt * a12},
        {0.5f * dt * a21 * primaryResistanceOhms,
         1.0f + 0.5f * dt * a22 * secondaryResistanceOhms,
         -0.5f * dt * a21, -0.5f * dt * a22},
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
    float x[4] = {projectedPrimaryCurrent, projectedSecondaryCurrent,
                  projectedPrimaryVoltage, projectedSecondaryVoltage};
    if (!solveLinear4x4(system, b, x)) return {};
    projectedPrimaryCurrent = x[0];
    projectedSecondaryCurrent = x[1];
    projectedPrimaryVoltage = x[2];
    projectedSecondaryVoltage = x[3];
    if (!std::isfinite(projectedPrimaryCurrent) ||
        !std::isfinite(projectedSecondaryCurrent) ||
        !std::isfinite(projectedPrimaryVoltage) ||
        !std::isfinite(projectedSecondaryVoltage)) {
      return {};
    }
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
  if (!std::isfinite(projected.primaryVoltage) ||
      !std::isfinite(projected.secondaryVoltage) ||
      !std::isfinite(projected.primaryCurrent) ||
      !std::isfinite(projected.secondaryCurrent)) {
    reset();
    return {};
  }
  primaryVoltage = projected.primaryVoltage;
  secondaryVoltage = projected.secondaryVoltage;
  primaryCurrent = projected.primaryCurrent;
  secondaryCurrent = projected.secondaryCurrent;
  return projected;
}

void Compressor::setFs(float newFs) {
  fs = newFs;
  setTimes(attackMs, releaseMs);
}

void Compressor::setTimes(float aMs, float rMs) {
  attackMs = aMs;
  releaseMs = rMs;
  atkCoeff = std::exp(-1.0f / (fs * (attackMs / 1000.0f)));
  relCoeff = std::exp(-1.0f / (fs * (releaseMs / 1000.0f)));
  gainAtkCoeff = std::exp(-1.0f / (fs * (attackMs / 1000.0f)));
  gainRelCoeff = std::exp(-1.0f / (fs * (releaseMs / 1000.0f)));
}

void Compressor::reset() {
  env = 0.0f;
  gainDb = 0.0f;
}

float Compressor::process(float x) {
  float a = std::fabs(x);
  if (a > env) {
    env = atkCoeff * env + (1.0f - atkCoeff) * a;
  } else {
    env = relCoeff * env + (1.0f - relCoeff) * a;
  }

  float levelDb = lin2db(env);
  float targetGrDb = 0.0f;
  if (levelDb > thresholdDb) {
    float over = levelDb - thresholdDb;
    float compressedOver = over / ratio;
    float outDb = thresholdDb + compressedOver;
    targetGrDb = outDb - levelDb;
  }

  if (targetGrDb < gainDb) {
    gainDb = gainAtkCoeff * gainDb + (1.0f - gainAtkCoeff) * targetGrDb;
  } else {
    gainDb = gainRelCoeff * gainDb + (1.0f - gainRelCoeff) * targetGrDb;
  }

  return x * db2lin(gainDb);
}

float Saturator::process(float x) {
  float yd = std::tanh(drive * x) / std::tanh(drive);
  return (1.0f - mix) * x + mix * yd;
}

static inline float softClip(float x,
                             float t = kRadioSoftClipThresholdDefault) {
  float ax = std::fabs(x);
  if (ax <= t) return x;
  float s = (x < 0.0f) ? -1.0f : 1.0f;
  float u = (ax - t) / (1.0f - t);
  float y = t + (1.0f - std::exp(-u)) * (1.0f - t);
  return s * y;
}

static inline float asymmetricSaturate(float x,
                                       float drive,
                                       float bias,
                                       float asymT) {
  auto shapeHalf = [](float v, float d) {
    return std::tanh(d * v) / std::max(1e-6f, d);
  };
  float shifted = x + bias;
  float posDrive = drive * std::max(0.25f, 1.0f - 0.10f * asymT);
  float negDrive = drive * (1.0f + 0.18f * asymT);
  float y = (shifted >= 0.0f) ? shapeHalf(shifted, posDrive)
                              : shapeHalf(shifted, negDrive);
  float base = (bias >= 0.0f) ? shapeHalf(bias, posDrive)
                              : shapeHalf(bias, negDrive);
  return y - base;
}

void NoiseHum::setFs(float newFs, float noiseBwHz) {
  fs = newFs;
  noiseLpHz = (noiseBwHz > 0.0f) ? noiseBwHz : noiseLpHz;
  float safeLp = std::clamp(noiseLpHz, noiseHpHz + 200.0f, fs * 0.45f);
  hp.setHighpass(fs, noiseHpHz, filterQ);
  lp.setLowpass(fs, safeLp, filterQ);
  crackleHp.setHighpass(fs, noiseHpHz, filterQ);
  crackleLp.setLowpass(fs, safeLp, filterQ);
  hp.reset();
  lp.reset();
  crackleHp.reset();
  crackleLp.reset();
  scAtk = std::exp(-1.0f / (fs * (scAttackMs / 1000.0f)));
  scRel = std::exp(-1.0f / (fs * (scReleaseMs / 1000.0f)));
  crackleDecay = std::exp(-1.0f / (fs * (crackleDecayMs / 1000.0f)));
}

void NoiseHum::reset() {
  humPhase = 0.0f;
  scEnv = 0.0f;
  crackleEnv = 0.0f;
  pinkFast = 0.0f;
  pinkSlow = 0.0f;
  brown = 0.0f;
  hissDrift = 0.0f;
  hissDriftSlow = 0.0f;
  hp.reset();
  lp.reset();
  crackleHp.reset();
  crackleLp.reset();
}

float NoiseHum::process(const NoiseInput& in) {
  float programAbs = std::fabs(in.programSample);
  if (programAbs > scEnv) {
    scEnv = scAtk * scEnv + (1.0f - scAtk) * programAbs;
  } else {
    scEnv = scRel * scEnv + (1.0f - scRel) * programAbs;
  }
  float maskT = clampf(scEnv / sidechainMaskRef, 0.0f, 1.0f);
  float hissMask = 1.0f - hissMaskDepth * maskT;
  float burstMask = 1.0f - burstMaskDepth * maskT;

  float white = dist(rng);
  pinkFast = pinkFastPole * pinkFast + (1.0f - pinkFastPole) * white;
  pinkSlow = pinkSlowPole * pinkSlow + (1.0f - pinkSlowPole) * white;
  brown = clampf(brown + brownStep * white, -1.0f, 1.0f);
  hissDrift = hissDriftPole * hissDrift + hissDriftNoise * dist(rng);
  hissDriftSlow =
      hissDriftSlowPole * hissDriftSlow + hissDriftSlowNoise * dist(rng);
  float n = whiteMix * white + pinkFastMix * pinkFast +
            pinkDifferenceMix * (pinkSlow - pinkFastSubtract * pinkFast) +
            brownMix * brown;
  n *= hissBase + hissDriftDepth * hissDrift;
  n += hissDriftSlowMix * hissDriftSlow;
  n = hp.process(n);
  n = lp.process(n);
  n *= in.noiseAmp * hissMask;

  float c = 0.0f;
  if (in.crackleRate > 0.0f && in.crackleAmp > 0.0f && fs > 0.0f) {
    float chance = in.crackleRate / fs;
    if (dist01(rng) < chance) {
      crackleEnv = 1.0f;
    }
    float raw = dist(rng) * crackleEnv;
    crackleEnv *= crackleDecay;
    raw = crackleHp.process(raw);
    raw = crackleLp.process(raw);
    c = raw * in.crackleAmp * burstMask;
  }

  float h = 0.0f;
  if (in.humToneEnabled && in.humAmp > 0.0f && fs > 0.0f) {
    humPhase += kRadioTwoPi * (humHz / fs);
    if (humPhase > kRadioTwoPi) humPhase -= kRadioTwoPi;
    h = std::sin(humPhase) + humSecondHarmonicMix * std::sin(2.0f * humPhase);
    h *= in.humAmp * hissMask;
  }

  return n + c + h;
}

void AMDetector::init(float newFs, float newBw, float newTuneHz) {
  fs = newFs;
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
  float audioCap = detectorPlateCouplingCapFarads;
  float avcCap = avcFilterCapFarads;
  float audioChargeSeconds = audioChargeResistanceOhms * audioCap;
  float audioReleaseSeconds = audioDischargeResistanceOhms * audioCap;
  float avcChargeSeconds = avcChargeResistanceOhms * avcCap;
  float avcReleaseSeconds = avcDischargeResistanceOhms * avcCap;
  audioChargeCoeff = std::exp(-1.0f / (fs * audioChargeSeconds));
  audioReleaseCoeff = std::exp(-1.0f / (fs * audioReleaseSeconds));
  avcChargeCoeff = std::exp(-1.0f / (fs * avcChargeSeconds));
  avcReleaseCoeff = std::exp(-1.0f / (fs * avcReleaseSeconds));
  setBandwidth(newBw, newTuneHz);
  reset();
}

void AMDetector::setBandwidth(float newBw, float newTuneHz) {
  bwHz = newBw;
  tuneOffsetHz = newTuneHz;
  float audioPostLpHz = std::clamp(0.48f * std::max(bwHz, 1.0f), 1200.0f,
                                   std::min(6000.0f, 0.12f * fs));
  float lowSenseBound = std::max(40.0f, senseLowHz);
  float highSenseBound = std::max(lowSenseBound + 180.0f, senseHighHz);
  float ifCenter = 0.5f * (lowSenseBound + highSenseBound);
  float afcOffset =
      std::clamp(0.18f * (highSenseBound - lowSenseBound), 120.0f,
                 std::max(120.0f, 0.30f * (highSenseBound - lowSenseBound)));
  float lowSenseHz =
      std::clamp(ifCenter - afcOffset, lowSenseBound, highSenseBound - 180.0f);
  float highSenseHz =
      std::clamp(ifCenter + afcOffset, lowSenseHz + 120.0f, highSenseBound);
  float afcLpHz = (afcSenseLpHz > 0.0f)
                      ? std::clamp(afcSenseLpHz, 5.0f, 180.0f)
                      : 45.0f;
  afcLowSense.setBandpass(fs, lowSenseHz, 1.10f);
  afcHighSense.setBandpass(fs, highSenseHz, 1.10f);
  afcErrorLp.setLowpass(fs, afcLpHz, kRadioBiquadQ);
  audioPostLp1.setLowpass(fs, audioPostLpHz, kRadioBiquadQ);
  audioPostLp2.setLowpass(fs, audioPostLpHz, kRadioBiquadQ);
}

void AMDetector::setSenseWindow(float lowHz, float highHz) {
  senseLowHz = lowHz;
  senseHighHz = highHz;
  if (fs > 0.0f) {
    setBandwidth(bwHz, tuneOffsetHz);
  }
}

void AMDetector::reset() {
  audioRect = 0.0f;
  avcRect = 0.0f;
  audioEnv = 0.0f;
  avcEnv = 0.0f;
  afcError = 0.0f;
  afcLowSense.reset();
  afcHighSense.reset();
  afcErrorLp.reset();
  audioPostLp1.reset();
  audioPostLp2.reset();
}

void Radio1938::CalibrationStageMetrics::clearAccumulators() {
  sampleCount = 0;
  rmsIn = 0.0;
  rmsOut = 0.0;
  meanIn = 0.0;
  meanOut = 0.0;
  peakIn = 0.0f;
  peakOut = 0.0f;
  crestIn = 0.0f;
  crestOut = 0.0f;
  spectralCentroidHz = 0.0f;
  bandwidth3dBHz = 0.0f;
  bandwidth6dBHz = 0.0f;
  clipCountIn = 0;
  clipCountOut = 0;
  inSum = 0.0;
  outSum = 0.0;
  inSumSq = 0.0;
  outSumSq = 0.0;
  bandEnergy.fill(0.0);
  fftBinEnergy.fill(0.0);
  fftTimeBuffer.fill(0.0f);
  fftFill = 0;
  fftBlockCount = 0;
}

void Radio1938::CalibrationStageMetrics::resetMeasurementState() {
  fftTimeBuffer.fill(0.0f);
  fftFill = 0;
}

static void fftInPlace(
    std::array<std::complex<float>, kRadioCalibrationFftSize>& bins) {
  const size_t n = bins.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(bins[i], bins[j]);
    }
  }

  for (size_t len = 2; len <= n; len <<= 1) {
    float angle = -kRadioTwoPi / static_cast<float>(len);
    std::complex<float> wLen(std::cos(angle), std::sin(angle));
    for (size_t i = 0; i < n; i += len) {
      std::complex<float> w(1.0f, 0.0f);
      size_t half = len >> 1;
      for (size_t j = 0; j < half; ++j) {
        std::complex<float> u = bins[i + j];
        std::complex<float> v = bins[i + j + half] * w;
        bins[i + j] = u + v;
        bins[i + j + half] = u - v;
        w *= wLen;
      }
    }
  }
}

static void accumulateCalibrationSpectrum(
    Radio1938::CalibrationStageMetrics& stage,
    float sampleRate,
    bool flushPartial) {
  if (stage.fftFill == 0) return;
  if (!flushPartial && stage.fftFill < kRadioCalibrationFftSize) return;

  std::array<std::complex<float>, kRadioCalibrationFftSize> bins{};
  const auto& window = radioCalibrationWindow();
  for (size_t i = 0; i < kRadioCalibrationFftSize; ++i) {
    float sample = (i < stage.fftFill) ? stage.fftTimeBuffer[i] : 0.0f;
    bins[i] = std::complex<float>(sample * window[i], 0.0f);
  }
  fftInPlace(bins);

  const auto& edges = radioCalibrationBandEdgesHz();
  float binHz = sampleRate / static_cast<float>(kRadioCalibrationFftSize);
  for (size_t i = 1; i < kRadioCalibrationFftBinCount; ++i) {
    float hz = static_cast<float>(i) * binHz;
    double energy = std::norm(bins[i]);
    stage.fftBinEnergy[i] += energy;
    for (size_t band = 0; band < kRadioCalibrationBandCount; ++band) {
      if (hz >= edges[band] && hz < edges[band + 1]) {
        stage.bandEnergy[band] += energy;
        break;
      }
    }
  }

  stage.fftBlockCount++;
  stage.fftTimeBuffer.fill(0.0f);
  stage.fftFill = 0;
}

void Radio1938::CalibrationState::reset() {
  totalSamples = 0;
  preLimiterClipCount = 0;
  postLimiterClipCount = 0;
  limiterActiveSamples = 0;
  limiterDutyCycle = 0.0f;
  limiterAverageGainReduction = 0.0f;
  limiterMaxGainReduction = 0.0f;
  limiterAverageGainReductionDb = 0.0f;
  limiterMaxGainReductionDb = 0.0f;
  limiterGainReductionSum = 0.0;
  limiterGainReductionDbSum = 0.0;
  validationSampleCount = 0;
  driverGridPositiveSamples = 0;
  outputGridPositiveSamples = 0;
  outputGridAPositiveSamples = 0;
  outputGridBPositiveSamples = 0;
  driverGridPositiveFraction = 0.0f;
  outputGridPositiveFraction = 0.0f;
  outputGridAPositiveFraction = 0.0f;
  outputGridBPositiveFraction = 0.0f;
  maxMixerPlateCurrentAmps = 0.0f;
  maxReceiverPlateCurrentAmps = 0.0f;
  maxDriverPlateCurrentAmps = 0.0f;
  maxOutputPlateCurrentAAmps = 0.0f;
  maxOutputPlateCurrentBAmps = 0.0f;
  interstageSecondarySumSq = 0.0;
  interstageSecondaryRmsVolts = 0.0f;
  interstageSecondaryPeakVolts = 0.0f;
  maxSpeakerSecondaryVolts = 0.0f;
  maxSpeakerReferenceRatio = 0.0f;
  maxDigitalOutput = 0.0f;
  validationDriverGridPositive = false;
  validationFailed = false;
  validationOutputGridPositive = false;
  validationOutputGridAPositive = false;
  validationOutputGridBPositive = false;
  validationSpeakerOverReference = false;
  validationInterstageSecondary = false;
  validationDcShift = false;
  validationDigitalClip = false;
  for (auto& stage : stages) {
    stage.clearAccumulators();
  }
}

void Radio1938::CalibrationState::resetMeasurementState() {
  for (auto& stage : stages) {
    stage.resetMeasurementState();
  }
}

static void updateStageCalibration(Radio1938& radio,
                                   StageId id,
                                   float in,
                                   float out) {
  if (!radio.calibration.enabled) return;
  auto& stage =
      radio.calibration.stages[static_cast<size_t>(id)];
  float clipThreshold = 1.0f;
  if (id == StageId::Power) {
    clipThreshold = std::max(radio.output.digitalReferenceSpeakerVoltsPeak, 1e-3f);
  }
  stage.sampleCount++;
  stage.inSum += static_cast<double>(in);
  stage.outSum += static_cast<double>(out);
  stage.inSumSq += static_cast<double>(in) * static_cast<double>(in);
  stage.outSumSq += static_cast<double>(out) * static_cast<double>(out);
  stage.peakIn = std::max(stage.peakIn, std::fabs(in));
  stage.peakOut = std::max(stage.peakOut, std::fabs(out));
  if (std::fabs(in) > clipThreshold) stage.clipCountIn++;
  if (std::fabs(out) > clipThreshold) stage.clipCountOut++;
  if (stage.fftFill < stage.fftTimeBuffer.size()) {
    stage.fftTimeBuffer[stage.fftFill++] = out;
  }
  if (stage.fftFill == stage.fftTimeBuffer.size()) {
    accumulateCalibrationSpectrum(stage, radio.sampleRate, false);
  }
}

void Radio1938::CalibrationStageMetrics::updateSnapshot(float sampleRate) {
  accumulateCalibrationSpectrum(*this, sampleRate, true);
  if (sampleCount == 0) return;
  double invCount = 1.0 / static_cast<double>(sampleCount);
  meanIn = inSum * invCount;
  meanOut = outSum * invCount;
  rmsIn = std::sqrt(inSumSq * invCount);
  rmsOut = std::sqrt(outSumSq * invCount);
  crestIn =
      (rmsIn > 1e-12) ? peakIn / static_cast<float>(rmsIn) : 0.0f;
  crestOut =
      (rmsOut > 1e-12) ? peakOut / static_cast<float>(rmsOut) : 0.0f;

  double totalEnergy = 0.0;
  double weightedHz = 0.0;
  double maxEnergy = 0.0;
  bandwidth3dBHz = 0.0f;
  bandwidth6dBHz = 0.0f;
  float binHz = sampleRate / static_cast<float>(kRadioCalibrationFftSize);
  for (size_t i = 1; i < fftBinEnergy.size(); ++i) {
    double energy = fftBinEnergy[i];
    float hz = static_cast<float>(i) * binHz;
    totalEnergy += energy;
    weightedHz += energy * hz;
    maxEnergy = std::max(maxEnergy, energy);
  }
  spectralCentroidHz = (totalEnergy > 1e-18) ? static_cast<float>(weightedHz / totalEnergy) : 0.0f;
  if (maxEnergy <= 0.0) return;

  double threshold3dB = maxEnergy * std::pow(10.0, -3.0 / 10.0);
  double threshold6dB = maxEnergy * std::pow(10.0, -6.0 / 10.0);
  for (size_t i = 1; i < fftBinEnergy.size(); ++i) {
    float hz = static_cast<float>(i) * binHz;
    if (fftBinEnergy[i] >= threshold3dB) {
      bandwidth3dBHz = hz;
    }
    if (fftBinEnergy[i] >= threshold6dB) {
      bandwidth6dBHz = hz;
    }
  }
}

static void updateCalibrationSnapshot(Radio1938& radio) {
  if (!radio.calibration.enabled) return;
  for (auto& stage : radio.calibration.stages) {
    stage.updateSnapshot(radio.sampleRate);
  }

  if (radio.calibration.totalSamples > 0) {
    float invCount = 1.0f / static_cast<float>(radio.calibration.totalSamples);
    radio.calibration.limiterDutyCycle =
        radio.calibration.limiterActiveSamples * invCount;
    radio.calibration.limiterAverageGainReduction =
        static_cast<float>(radio.calibration.limiterGainReductionSum * invCount);
    radio.calibration.limiterAverageGainReductionDb =
        static_cast<float>(radio.calibration.limiterGainReductionDbSum * invCount);
  }

  if (radio.calibration.validationSampleCount > 0) {
    float invValidationCount =
        1.0f / static_cast<float>(radio.calibration.validationSampleCount);
    radio.calibration.driverGridPositiveFraction =
        radio.calibration.driverGridPositiveSamples * invValidationCount;
    radio.calibration.outputGridPositiveFraction =
        radio.calibration.outputGridPositiveSamples * invValidationCount;
    radio.calibration.outputGridAPositiveFraction =
        radio.calibration.outputGridAPositiveSamples * invValidationCount;
    radio.calibration.outputGridBPositiveFraction =
        radio.calibration.outputGridBPositiveSamples * invValidationCount;
    radio.calibration.interstageSecondaryRmsVolts =
        std::sqrt(radio.calibration.interstageSecondarySumSq *
                  invValidationCount);
  }

  const auto& receiverStage =
      radio.calibration.stages[static_cast<size_t>(StageId::ReceiverCircuit)];
  const auto& powerStage =
      radio.calibration.stages[static_cast<size_t>(StageId::Power)];
  radio.calibration.validationDriverGridPositive =
      radio.calibration.driverGridPositiveFraction > 0.01f;
  radio.calibration.validationOutputGridAPositive =
      radio.calibration.outputGridAPositiveFraction > 0.002f;
  radio.calibration.validationOutputGridBPositive =
      radio.calibration.outputGridBPositiveFraction > 0.002f;
  radio.calibration.validationOutputGridPositive =
      radio.calibration.validationOutputGridAPositive ||
      radio.calibration.validationOutputGridBPositive;
  radio.calibration.validationSpeakerOverReference =
      radio.calibration.maxSpeakerReferenceRatio > 1.10f;
  radio.calibration.validationInterstageSecondary =
      radio.calibration.interstageSecondaryPeakVolts >
      4.0f * std::max(std::fabs(radio.power.outputTubeBiasVolts), 1.0f);
  radio.calibration.validationDcShift =
      (std::fabs(receiverStage.meanOut) >
           std::max(0.02, 0.10 * receiverStage.rmsOut)) ||
      (std::fabs(powerStage.meanOut) >
           std::max(0.10, 0.05 * powerStage.peakOut));
  radio.calibration.validationDigitalClip =
      radio.calibration.maxDigitalOutput > 1.0f ||
      radio.diagnostics.outputClip || radio.diagnostics.finalLimiterActive;
  radio.calibration.validationFailed =
      radio.calibration.validationDriverGridPositive ||
      radio.calibration.validationOutputGridPositive ||
      radio.calibration.validationSpeakerOverReference ||
      radio.calibration.validationInterstageSecondary ||
      radio.calibration.validationDcShift ||
      radio.calibration.validationDigitalClip;
}

float AMDetector::process(const AMDetectorSampleInput& in) {
  float ifSample = in.signal;
  if (in.ifNoiseAmp > 0.0f) {
    ifSample += dist(rng) * in.ifNoiseAmp;
  }

  float afcLow = std::fabs(afcLowSense.process(ifSample));
  float afcHigh = std::fabs(afcHighSense.process(ifSample));
  float afcDen = std::max(afcLow + afcHigh, 1e-6f);
  float rawAfcError = (afcHigh - afcLow) / afcDen;
  afcError = afcErrorLp.process(rawAfcError);

  audioRect = diodeJunctionRectify(ifSample, audioDiodeDrop,
                                   audioJunctionSlopeVolts);
  avcRect = diodeJunctionRectify(ifSample, avcDiodeDrop,
                                 avcJunctionSlopeVolts);

  if (audioRect > audioEnv) {
    audioEnv = audioChargeCoeff * audioEnv +
               (1.0f - audioChargeCoeff) * audioRect;
  } else {
    audioEnv = audioReleaseCoeff * audioEnv +
               (1.0f - audioReleaseCoeff) * audioRect;
  }

  if (avcRect > avcEnv) {
    avcEnv = avcChargeCoeff * avcEnv + (1.0f - avcChargeCoeff) * avcRect;
  } else {
    avcEnv =
        avcReleaseCoeff * avcEnv + (1.0f - avcReleaseCoeff) * avcRect;
  }
  float audioOut = audioPostLp1.process(audioEnv - avcEnv);
  audioOut = audioPostLp2.process(audioOut);
  return audioOut;
}

void SpeakerSim::init(float fs) {
  float suspensionHzDerived =
      suspensionHz * (1.0f + 0.45f * coneMassTolerance -
                      0.65f * suspensionComplianceTolerance);
  float coneBodyHzDerived =
      coneBodyHz * (1.0f + 0.22f * coneMassTolerance +
                    0.16f * voiceCoilTolerance);
  suspensionRes.setPeaking(fs, suspensionHzDerived, suspensionQ, suspensionGainDb);
  coneBody.setPeaking(fs, coneBodyHzDerived, coneBodyQ, coneBodyGainDb);
  upperBreakup = Biquad{};
  coneDip = Biquad{};
  if (topLpHz > 0.0f) {
    float topLpHzDerived = topLpHz / (1.0f + 0.40f * voiceCoilTolerance);
    topLp.setLowpass(fs, topLpHzDerived, filterQ);
  } else {
    topLp = Biquad{};
  }
  hfLossLp = Biquad{};
  excursionAtk = std::exp(-1.0f / (fs * 0.010f));
  excursionRel = std::exp(-1.0f / (fs * 0.120f));
}

void SpeakerSim::reset() {
  suspensionRes.reset();
  coneBody.reset();
  upperBreakup.reset();
  coneDip.reset();
  topLp.reset();
  hfLossLp.reset();
  excursionEnv = 0.0f;
}

float SpeakerSim::process(float x, bool& clipped) {
  float y = x * std::max(drive, 0.0f);
  y = suspensionRes.process(y);
  y = coneBody.process(y);
  if (topLpHz > 0.0f) {
    y = topLp.process(y);
  }

  float a = std::fabs(y);
  if (a > excursionEnv) {
    excursionEnv = excursionAtk * excursionEnv + (1.0f - excursionAtk) * a;
  } else {
    excursionEnv = excursionRel * excursionEnv + (1.0f - excursionRel) * a;
  }

  float excursionT =
      clampf(excursionEnv / std::max(excursionRef, 1e-6f), 0.0f, 1.0f);
  float complianceGain = 1.0f - complianceLossDepth * excursionT;
  y *= std::max(0.70f, complianceGain);
  clipped = limit > 0.0f && std::fabs(y) > limit;
  if (limit > 0.0f && limit < 1.0f) {
    return softClip(y, limit);
  }
  return y;
}

float RadioTuningNode::applyFilters(Radio1938& radio, float tuneHz, float bwHz) {
  auto& tuning = radio.tuning;
  auto& frontEnd = radio.frontEnd;
  float safeBw = bwHz;
  float preBw = safeBw * tuning.preBwScale;
  float rfBw = safeBw * tuning.postBwScale;
  RadioIFStripNode::setBandwidth(radio, safeBw, tuneHz);
  float rfCenterHz = radio.ifStrip.sourceCarrierHz;
  auto resonantCapacitanceFarads = [](float freqHz, float inductanceHenries) {
    float omega = kRadioTwoPi * std::max(freqHz, 1.0f);
    return 1.0f / std::max(omega * omega * std::max(inductanceHenries, 1e-9f),
                           1e-18f);
  };
  auto seriesResistanceForBandwidth = [](float inductanceHenries,
                                         float bandwidthHz) {
    return std::max(kRadioTwoPi * std::max(inductanceHenries, 1e-9f) *
                        std::max(bandwidthHz, 1.0f),
                    1e-4f);
  };

  frontEnd.antennaCapacitanceFarads =
      resonantCapacitanceFarads(rfCenterHz, frontEnd.antennaInductanceHenries);
  frontEnd.rfCapacitanceFarads =
      resonantCapacitanceFarads(rfCenterHz, frontEnd.rfInductanceHenries);
  frontEnd.antennaSeriesResistanceOhms =
      seriesResistanceForBandwidth(frontEnd.antennaInductanceHenries, preBw);
  frontEnd.rfSeriesResistanceOhms =
      seriesResistanceForBandwidth(frontEnd.rfInductanceHenries, rfBw);
  frontEnd.antennaTank.configure(
      radio.sampleRate, frontEnd.antennaInductanceHenries,
      frontEnd.antennaCapacitanceFarads,
      frontEnd.antennaSeriesResistanceOhms + frontEnd.antennaLoadResistanceOhms,
      frontEnd.antennaLoadResistanceOhms, 8);
  frontEnd.rfTank.configure(radio.sampleRate, frontEnd.rfInductanceHenries,
                            frontEnd.rfCapacitanceFarads,
                            frontEnd.rfSeriesResistanceOhms +
                                frontEnd.rfLoadResistanceOhms,
                            frontEnd.rfLoadResistanceOhms, 8);

  frontEnd.preLpfIn.setBandpass(
      radio.sampleRate, rfCenterHz, std::max(0.35f, rfCenterHz / preBw));
  frontEnd.preLpfOut.setBandpass(
      radio.sampleRate, rfCenterHz, std::max(0.35f, rfCenterHz / rfBw));

  tuning.tunedBw = safeBw;
  return safeBw;
}

void RadioTuningNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& tuning = radio.tuning;
  tuning.afcCorrectionHz = 0.0f;
  initCtx.tunedBw = applyFilters(radio, tuning.tuneOffsetHz, radio.bwHz);
  tuning.tuneAppliedHz = tuning.tuneOffsetHz;
  tuning.bwAppliedHz = radio.bwHz;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = radio.bwHz;
}

void RadioTuningNode::reset(Radio1938& radio) {
  auto& tuning = radio.tuning;
  tuning.afcCorrectionHz = 0.0f;
  tuning.tuneAppliedHz = tuning.tuneOffsetHz;
  tuning.bwAppliedHz = radio.bwHz;
  tuning.tuneSmoothedHz = tuning.tuneOffsetHz;
  tuning.bwSmoothedHz = radio.bwHz;
}

void RadioTuningNode::prepare(Radio1938& radio,
                              RadioBlockControl& block,
                              uint32_t frames) {
  auto& tuning = radio.tuning;
  auto& demod = radio.demod;
  auto& noiseRuntime = radio.noiseRuntime;
  float rate = std::max(1.0f, radio.sampleRate);
  float tick =
      1.0f - std::exp(-static_cast<float>(frames) / (rate * tuning.smoothTau));
  float effectiveTuneHz = tuning.tuneOffsetHz;
  if (tuning.magneticTuningEnabled) {
    effectiveTuneHz += tuning.afcCorrectionHz;
  }
  tuning.tuneSmoothedHz += tick * (effectiveTuneHz - tuning.tuneSmoothedHz);
  tuning.bwSmoothedHz += tick * (radio.bwHz - tuning.bwSmoothedHz);

  float safeBw = tuning.bwSmoothedHz;
  float bwHalf = 0.5f * std::max(1.0f, safeBw);
  block.tuneNorm = clampf(tuning.tuneSmoothedHz / bwHalf, -1.0f, 1.0f);

  if (std::fabs(tuning.tuneSmoothedHz - tuning.tuneAppliedHz) > tuning.updateEps ||
      std::fabs(tuning.bwSmoothedHz - tuning.bwAppliedHz) > tuning.updateEps) {
    float tunedBw = applyFilters(radio, tuning.tuneSmoothedHz, tuning.bwSmoothedHz);
    tuning.tuneAppliedHz = tuning.tuneSmoothedHz;
    tuning.bwAppliedHz = tuning.bwSmoothedHz;
    demod.am.setBandwidth(tunedBw, tuning.tuneSmoothedHz);
    noiseRuntime.hum.setFs(radio.sampleRate, tunedBw);
  }
}

void RadioInputNode::init(Radio1938& radio, RadioInitContext&) {
  auto& input = radio.input;
  input.autoEnvAtk =
      std::exp(-1.0f / (radio.sampleRate * (input.autoEnvAttackMs / 1000.0f)));
  input.autoEnvRel =
      std::exp(-1.0f / (radio.sampleRate * (input.autoEnvReleaseMs / 1000.0f)));
  input.autoGainAtk =
      std::exp(-1.0f / (radio.sampleRate * (input.autoGainAttackMs / 1000.0f)));
  input.autoGainRel =
      std::exp(-1.0f / (radio.sampleRate * (input.autoGainReleaseMs / 1000.0f)));
  float sourceR = input.sourceResistanceOhms;
  float loadR = input.inputResistanceOhms;
  input.sourceDivider = loadR / (sourceR + loadR);
  if (input.couplingCapFarads > 0.0f) {
    float hpHz = 1.0f / (kRadioTwoPi * (sourceR + loadR) * input.couplingCapFarads);
    input.sourceCouplingHp.setHighpass(radio.sampleRate, hpHz, kRadioBiquadQ);
  } else {
    input.sourceCouplingHp = Biquad{};
  }
}

void RadioInputNode::reset(Radio1938& radio) {
  radio.input.autoEnv = 0.0f;
  radio.input.autoGainDb = 0.0f;
  radio.input.sourceCouplingHp.reset();
}

float RadioInputNode::process(Radio1938& radio,
                              float x,
                              const RadioSampleContext&) {
  auto& input = radio.input;
  if (radio.sourceFrame.mode == SourceInputMode::RealRf) {
    x *= input.sourceDivider;
    if (input.couplingCapFarads > 0.0f) {
      x = input.sourceCouplingHp.process(x);
    }
  }
  x *= radio.globals.inputPad;
  if (!radio.globals.enableAutoLevel) return x;

  float ax = std::fabs(x);
  if (ax > input.autoEnv) {
    input.autoEnv = input.autoEnvAtk * input.autoEnv + (1.0f - input.autoEnvAtk) * ax;
  } else {
    input.autoEnv = input.autoEnvRel * input.autoEnv + (1.0f - input.autoEnvRel) * ax;
  }
  float envDb = lin2db(input.autoEnv);
  float targetBoostDb =
      std::clamp(radio.globals.autoTargetDb - envDb, 0.0f,
                 radio.globals.autoMaxBoostDb);
  if (targetBoostDb < input.autoGainDb) {
    input.autoGainDb = input.autoGainAtk * input.autoGainDb +
                       (1.0f - input.autoGainAtk) * targetBoostDb;
  } else {
    input.autoGainDb = input.autoGainRel * input.autoGainDb +
                       (1.0f - input.autoGainRel) * targetBoostDb;
  }
  return x * db2lin(input.autoGainDb);
}

void RadioControlBusNode::init(Radio1938&, RadioInitContext&) {}

void RadioControlBusNode::reset(Radio1938& radio) {
  radio.controlSense.reset();
  radio.controlBus.reset();
}

void RadioAVCNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& controlBus = radio.controlBus;
  const auto& controlSense = radio.controlSense;
  controlBus.controlVoltage =
      clampf(controlSense.controlVoltageSense, 0.0f, 1.25f);
}

void RadioAFCNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& tuning = radio.tuning;
  const auto& controlSense = radio.controlSense;
  if (!tuning.magneticTuningEnabled || tuning.afcMaxCorrectionHz <= 0.0f ||
      tuning.afcResponseMs <= 0.0f) {
    tuning.afcCorrectionHz = 0.0f;
    return;
  }

  float rate = std::max(radio.sampleRate, 1.0f);
  float afcSeconds = tuning.afcResponseMs * 0.001f;
  float afcTick = 1.0f - std::exp(-1.0f / (rate * afcSeconds));
  float error = controlSense.tuningErrorSense;
  if (std::fabs(error) < tuning.afcDeadband) error = 0.0f;
  float captureT =
      1.0f - clampf(std::fabs(tuning.tuneOffsetHz) /
                        std::max(tuning.afcCaptureHz, 1e-6f),
                    0.0f, 1.0f);
  float signalT =
      clampf(controlSense.controlVoltageSense / 0.85f, 0.0f, 1.0f);
  float afcTarget =
      -error * tuning.afcMaxCorrectionHz * captureT * signalT;
  tuning.afcCorrectionHz += afcTick * (afcTarget - tuning.afcCorrectionHz);
  tuning.afcCorrectionHz =
      clampf(tuning.afcCorrectionHz, -tuning.afcMaxCorrectionHz,
             tuning.afcMaxCorrectionHz);
}

void RadioControlBusNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& controlBus = radio.controlBus;
  const auto& controlSense = radio.controlSense;
  const auto& power = radio.power;
  float supplyTarget =
      clampf((controlSense.powerSagSense - power.sagStart) /
                 std::max(1e-6f, power.sagEnd - power.sagStart),
             0.0f, 1.0f);
  controlBus.supplySag = supplyTarget;
}

void RadioFrontEndNode::init(Radio1938& radio, RadioInitContext&) {
  radio.frontEnd.hpf.setHighpass(radio.sampleRate, radio.frontEnd.inputHpHz,
                                 kRadioBiquadQ);
  radio.frontEnd.selectivityPeak.setPeaking(radio.sampleRate,
                                            radio.frontEnd.selectivityPeakHz,
                                            radio.frontEnd.selectivityPeakQ,
                                            radio.frontEnd.selectivityPeakGainDb);
}

void RadioFrontEndNode::reset(Radio1938& radio) {
  auto& frontEnd = radio.frontEnd;
  frontEnd.hpf.reset();
  frontEnd.preLpfIn.reset();
  frontEnd.preLpfOut.reset();
  frontEnd.selectivityPeak.reset();
  frontEnd.antennaTank.reset();
  frontEnd.rfTank.reset();
}

float RadioFrontEndNode::process(Radio1938& radio,
                                 float x,
                                 const RadioSampleContext&) {
  auto& frontEnd = radio.frontEnd;
  float rfHold = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float y = frontEnd.hpf.process(x);
  y = frontEnd.antennaTank.process(y);
  y *= frontEnd.rfGain * std::max(0.35f, 1.0f - frontEnd.avcGainDepth * rfHold);
  y = frontEnd.rfTank.process(y);
  y = frontEnd.selectivityPeak.process(y);
  return y;
}

void RadioMixerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& mixer = radio.mixer;
  float dcLoadResistanceOhms = 0.0f;
  if (mixer.plateCurrentAmps > 1e-6f &&
      mixer.plateSupplyVolts > mixer.plateDcVolts) {
    dcLoadResistanceOhms =
        (mixer.plateSupplyVolts - mixer.plateDcVolts) / mixer.plateCurrentAmps;
  }
  PentodeOperatingPoint op = solvePentodeOperatingPoint(
      mixer.plateSupplyVolts, mixer.screenVolts, dcLoadResistanceOhms,
      mixer.biasVolts, mixer.plateDcVolts, mixer.plateCurrentAmps,
      mixer.mutualConductanceSiemens, mixer.plateKneeVolts,
      mixer.gridSoftnessVolts, mixer.modelCutoffVolts);
  mixer.plateQuiescentVolts = op.plateVolts;
  mixer.plateQuiescentCurrentAmps = op.plateCurrentAmps;
  mixer.plateResistanceOhms = op.rpOhms;
  assert((std::fabs(mixer.plateQuiescentVolts - mixer.plateDcVolts) <=
          mixer.operatingPointToleranceVolts) &&
         "Mixer operating point diverged from the preset target");
}

void RadioMixerNode::reset(Radio1938&) {}

float RadioMixerNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext&) {
  // Real LO/mixing happens inside the oversampled IF core.
  (void)radio;
  return y;
}

static int computeIfOversampleFactor(float outputFs,
                                     float ifCenterHz,
                                     float bwHz) {
  float safeFs = std::max(outputFs, 1.0f);
  float safeBw = std::max(bwHz, 1.0f);
  float guardHz = std::max(16000.0f, 1.5f * safeBw + 12000.0f);
  float minInternalFs = (ifCenterHz + guardHz) / 0.48f;
  return std::max(8, static_cast<int>(std::ceil(minInternalFs / safeFs)));
}

static float selectSourceCarrierHz(float outputFs,
                                   float internalFs,
                                   float ifCenterHz,
                                   float bwHz) {
  float maxCarrierByIf = 0.48f * internalFs - ifCenterHz - 8000.0f;
  float audioSidebandHz = 0.48f * std::max(bwHz, 1.0f);
  float maxCarrierByOutput =
      0.5f * std::max(outputFs, 1.0f) - audioSidebandHz - 1600.0f;
  float maxCarrier = std::min(maxCarrierByIf, maxCarrierByOutput);
  if (maxCarrier <= 6000.0f) {
    return std::clamp(0.25f * std::max(outputFs, 1.0f), 3000.0f,
                      std::max(3000.0f, maxCarrierByOutput));
  }
  return std::clamp(0.62f * maxCarrier, 6000.0f, maxCarrier);
}

void RadioIFStripNode::init(Radio1938& radio, RadioInitContext&) {
  setBandwidth(radio, radio.bwHz, radio.tuning.tuneOffsetHz);
}

void RadioIFStripNode::reset(Radio1938& radio) {
  auto& ifStrip = radio.ifStrip;
  ifStrip.rfPhase = 0.0f;
  ifStrip.loPhase = 0.0f;
  ifStrip.prevSourceMode = SourceInputMode::ComplexEnvelope;
  ifStrip.prevSourceI = 0.0f;
  ifStrip.prevSourceQ = 0.0f;
  ifStrip.bp1.reset();
  ifStrip.bp2.reset();
  ifStrip.interstageTransformer.reset();
  ifStrip.outputTransformer.reset();
}

void RadioIFStripNode::setBandwidth(Radio1938& radio, float bwHz, float tuneHz) {
  auto& ifStrip = radio.ifStrip;
  auto& demod = radio.demod;
  auto resonantCapacitanceFarads = [](float freqHz, float inductanceHenries) {
    float omega = kRadioTwoPi * std::max(freqHz, 1.0f);
    return 1.0f / std::max(omega * omega * std::max(inductanceHenries, 1e-9f),
                           1e-18f);
  };
  auto seriesResistanceForBandwidth = [](float inductanceHenries,
                                         float bandwidthHz) {
    return std::max(kRadioTwoPi * std::max(inductanceHenries, 1e-9f) *
                        std::max(bandwidthHz, 1.0f),
                    1e-4f);
  };
  float sampleRate = std::max(radio.sampleRate, 1.0f);
  float safeBw = std::max(bwHz, ifStrip.ifMinBwHz);
  ifStrip.oversampleFactor =
      computeIfOversampleFactor(sampleRate, ifStrip.ifCenterHz, safeBw);
  ifStrip.internalSampleRate = sampleRate * ifStrip.oversampleFactor;
  ifStrip.sourceCarrierHz = selectSourceCarrierHz(
      sampleRate, ifStrip.internalSampleRate, ifStrip.ifCenterHz, safeBw);
  ifStrip.loFrequencyHz = ifStrip.sourceCarrierHz + ifStrip.ifCenterHz + tuneHz;

  float stageBandwidthHz = std::max(safeBw * 1.55f, 600.0f);
  float stageQ = std::max(0.35f, ifStrip.ifCenterHz / stageBandwidthHz);
  ifStrip.bp1.setBandpass(ifStrip.internalSampleRate, ifStrip.ifCenterHz, stageQ);
  ifStrip.bp2.setBandpass(ifStrip.internalSampleRate, ifStrip.ifCenterHz, stageQ);
  ifStrip.primaryCapacitanceFarads =
      resonantCapacitanceFarads(ifStrip.ifCenterHz,
                                ifStrip.primaryInductanceHenries);
  ifStrip.secondaryCapacitanceFarads =
      resonantCapacitanceFarads(ifStrip.ifCenterHz,
                                ifStrip.secondaryInductanceHenries);
  ifStrip.primaryResistanceOhms =
      seriesResistanceForBandwidth(ifStrip.primaryInductanceHenries,
                                   stageBandwidthHz);
  ifStrip.secondaryResistanceOhms =
      seriesResistanceForBandwidth(ifStrip.secondaryInductanceHenries,
                                   stageBandwidthHz);
  ifStrip.interstageTransformer.configure(
      ifStrip.internalSampleRate, ifStrip.primaryInductanceHenries,
      ifStrip.primaryCapacitanceFarads, ifStrip.primaryResistanceOhms,
      ifStrip.secondaryInductanceHenries, ifStrip.secondaryCapacitanceFarads,
      ifStrip.secondaryResistanceOhms + ifStrip.secondaryLoadResistanceOhms,
      ifStrip.interstageCouplingCoeff,
      ifStrip.secondaryLoadResistanceOhms, 8);
  ifStrip.outputTransformer.configure(
      ifStrip.internalSampleRate, ifStrip.primaryInductanceHenries,
      ifStrip.primaryCapacitanceFarads, ifStrip.primaryResistanceOhms,
      ifStrip.secondaryInductanceHenries, ifStrip.secondaryCapacitanceFarads,
      ifStrip.secondaryResistanceOhms + ifStrip.secondaryLoadResistanceOhms,
      ifStrip.outputCouplingCoeff,
      ifStrip.secondaryLoadResistanceOhms, 8);

  float senseLow = ifStrip.ifCenterHz - 0.5f * safeBw;
  float senseHigh = ifStrip.ifCenterHz + 0.5f * safeBw;
  demod.am.setSenseWindow(senseLow, senseHigh);
  if (demod.am.fs > 0.0f) {
    demod.am.setBandwidth(safeBw, tuneHz);
  }
}

float RadioIFStripNode::process(Radio1938& radio,
                                float y,
                                const RadioSampleContext&) {
  (void)radio;
  return y;
}

void RadioDemodNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& demod = radio.demod;
  demod.am.init(radio.ifStrip.internalSampleRate, initCtx.tunedBw,
                radio.tuning.tuneOffsetHz);
}

void RadioDemodNode::reset(Radio1938& radio) { radio.demod.am.reset(); }

static float processSuperhetSourceFrame(Radio1938& radio,
                                        float frontEndSample,
                                        const RadioSampleContext& ctx) {
  auto& frontEnd = radio.frontEnd;
  auto& mixer = radio.mixer;
  auto& ifStrip = radio.ifStrip;
  auto& demod = radio.demod.am;
  if (!ifStrip.enabled || ifStrip.internalSampleRate <= 0.0f ||
      ifStrip.oversampleFactor <= 0) {
    return radio.sourceFrame.i;
  }

  SourceInputMode mode = radio.sourceFrame.mode;
  float currI =
      (mode == SourceInputMode::RealRf) ? frontEndSample : radio.sourceFrame.i;
  float currQ = radio.sourceFrame.q;
  float prevI = ifStrip.prevSourceI;
  float prevQ = ifStrip.prevSourceQ;
  if (mode != ifStrip.prevSourceMode) {
    prevI = currI;
    prevQ = currQ;
  }
  float avcT = clampf(radio.controlBus.controlVoltage / 1.25f, 0.0f, 1.0f);
  float rfGain =
      frontEnd.rfGain * std::max(0.35f, 1.0f - frontEnd.avcGainDepth * avcT);
  float ifGain = ifStrip.stageGain;
  float rfStep =
      kRadioTwoPi * (ifStrip.sourceCarrierHz / ifStrip.internalSampleRate);
  float loStep =
      kRadioTwoPi * (ifStrip.loFrequencyHz / ifStrip.internalSampleRate);
  float noiseAmp =
      ctx.derived.demodIfNoiseAmp / std::sqrt(static_cast<float>(ifStrip.oversampleFactor));
  float audioAcc = 0.0f;

  for (int step = 0; step < ifStrip.oversampleFactor; ++step) {
    float t =
        static_cast<float>(step + 1) / static_cast<float>(ifStrip.oversampleFactor);
    float rf = 0.0f;
    if (mode == SourceInputMode::ComplexEnvelope) {
      float envI = prevI + (currI - prevI) * t;
      float envQ = prevQ + (currQ - prevQ) * t;
      rf = rfGain *
           (envI * std::cos(ifStrip.rfPhase) - envQ * std::sin(ifStrip.rfPhase));
      ifStrip.rfPhase = wrapPhase(ifStrip.rfPhase + rfStep);
    } else {
      rf = prevI + (currI - prevI) * t;
    }
    float lo = std::cos(ifStrip.loPhase);
    ifStrip.loPhase = wrapPhase(ifStrip.loPhase + loStep);
    float baseGridVolts =
        mixer.biasVolts - mixer.avcGridDriveVolts * avcT;
    float rfGridVolts = mixer.rfGridDriveVolts * rf;
    float oscillatorGridVolts = mixer.loGridDriveVolts * lo;
    float mixerPlateAcVolts = processConverterTubeStage(
        baseGridVolts, rfGridVolts, mixer.loGridBiasVolts,
        oscillatorGridVolts, 1.0f,
      mixer.plateQuiescentVolts,
        mixer.screenVolts, mixer.biasVolts, mixer.modelCutoffVolts,
        mixer.plateQuiescentCurrentAmps, mixer.mutualConductanceSiemens,
        mixer.acLoadResistanceOhms, mixer.plateKneeVolts,
        mixer.gridSoftnessVolts);
    if (radio.calibration.enabled) {
      float instantaneousMixerCurrent = deviceTubePlateCurrent(
          baseGridVolts + mixer.loGridBiasVolts + rfGridVolts +
              oscillatorGridVolts,
          mixer.plateQuiescentVolts, mixer.screenVolts, mixer.biasVolts,
          mixer.modelCutoffVolts, mixer.plateQuiescentVolts,
          mixer.screenVolts, mixer.plateQuiescentCurrentAmps,
          mixer.mutualConductanceSiemens, mixer.plateKneeVolts,
          mixer.gridSoftnessVolts);
      radio.calibration.maxMixerPlateCurrentAmps = std::max(
          radio.calibration.maxMixerPlateCurrentAmps,
          instantaneousMixerCurrent);
    }
    float ifGainControl =
        std::max(0.20f, 1.0f - ifStrip.avcGainDepth * avcT);
    float ifSample = mixerPlateAcVolts * ifGain * ifGainControl;
    ifSample = ifStrip.interstageTransformer.process(ifSample);
    ifSample = ifStrip.outputTransformer.process(ifSample);
    audioAcc += demod.process(AMDetectorSampleInput{ifSample, noiseAmp});
  }

  ifStrip.prevSourceMode = mode;
  ifStrip.prevSourceI = currI;
  ifStrip.prevSourceQ = currQ;
  return audioAcc / static_cast<float>(ifStrip.oversampleFactor);
}

float RadioDemodNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext& ctx) {
  auto& demod = radio.demod;
  y = processSuperhetSourceFrame(radio, y, ctx);
  radio.controlSense.controlVoltageSense =
      clampf(demod.am.avcEnv / std::max(demod.am.controlVoltageRef, 1e-6f),
             0.0f, 1.25f);
  radio.controlSense.tuningErrorSense = demod.am.afcError;
  return y;
}

void RadioReceiverCircuitNode::init(Radio1938& radio, RadioInitContext&) {
  auto& receiver = radio.receiverCircuit;
  if (!receiver.enabled || !receiver.tubeTriodeConnected) return;
  TriodeOperatingPoint op = solveTriodeOperatingPoint(
      receiver.tubePlateSupplyVolts, receiver.tubeLoadResistanceOhms,
      receiver.tubeBiasVolts, receiver.tubePlateDcVolts,
      receiver.tubePlateCurrentAmps, receiver.tubeMutualConductanceSiemens,
      receiver.tubeMu);
  receiver.tubeQuiescentPlateVolts = op.plateVolts;
  receiver.tubeQuiescentPlateCurrentAmps = op.plateCurrentAmps;
  receiver.tubePlateResistanceOhms = op.rpOhms;
  receiver.tubeTriodeModel = op.model;
  assert((std::fabs(receiver.tubeQuiescentPlateVolts -
                    receiver.tubePlateDcVolts) <=
          receiver.operatingPointToleranceVolts) &&
         "6J5 first-audio operating point diverged from the preset target");
}

void RadioReceiverCircuitNode::reset(Radio1938& radio) {
  auto& receiver = radio.receiverCircuit;
  receiver.couplingCapVoltage = 0.0f;
  receiver.gridVoltage = 0.0f;
  receiver.volumeControlTapVoltage = 0.0f;
  receiver.tubePlateVoltage = receiver.tubeQuiescentPlateVolts;
}

float RadioReceiverCircuitNode::process(Radio1938& radio,
                                        float y,
                                        const RadioSampleContext&) {
  auto& receiver = radio.receiverCircuit;
  if (!receiver.enabled) return y;
  float dt = 1.0f / std::max(radio.sampleRate, 1.0f);
  float totalResistance = receiver.volumeControlResistanceOhms;
  float wiperResistance = receiver.volumeControlPosition * totalResistance;
  float tapResistance = receiver.volumeControlTapResistanceOhms;
  float loudnessBlend = 0.0f;
  if (tapResistance > 0.0f) {
    loudnessBlend = clampf((tapResistance - wiperResistance) / tapResistance,
                           0.0f, 1.0f);
  }
  constexpr float kNodeLinkOhms = 1e-3f;
  auto addGroundBranch = [](float (&a)[3][3], int node, float resistanceOhms) {
    if (resistanceOhms <= 0.0f) return;
    a[node][node] += 1.0f / std::max(resistanceOhms, 1e-9f);
  };
  auto addSourceBranch = [](float (&a)[3][3], float (&b)[3], int node,
                            float resistanceOhms, float sourceVoltage) {
    if (resistanceOhms <= 0.0f) return;
    float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
    a[node][node] += conductance;
    b[node] += conductance * sourceVoltage;
  };
  auto addNodeBranch = [](float (&a)[3][3], int aNode, int bNode,
                          float resistanceOhms) {
    if (resistanceOhms <= 0.0f) return;
    float conductance = 1.0f / std::max(resistanceOhms, 1e-9f);
    a[aNode][aNode] += conductance;
    a[bNode][bNode] += conductance;
    a[aNode][bNode] -= conductance;
    a[bNode][aNode] -= conductance;
  };
  float a[3][3] = {};
  float b[3] = {};
  constexpr int kWiperNode = 0;
  constexpr int kTapNode = 1;
  constexpr int kGridNode = 2;
  if (wiperResistance >= tapResistance) {
    addSourceBranch(a, b, kWiperNode,
                    std::max(totalResistance - wiperResistance, kNodeLinkOhms),
                    y);
    addNodeBranch(a, kWiperNode, kTapNode,
                  std::max(wiperResistance - tapResistance, kNodeLinkOhms));
    addGroundBranch(a, kTapNode, std::max(tapResistance, kNodeLinkOhms));
  } else {
    addSourceBranch(a, b, kTapNode,
                    std::max(totalResistance - tapResistance, kNodeLinkOhms), y);
    addNodeBranch(a, kTapNode, kWiperNode,
                  std::max(tapResistance - wiperResistance, kNodeLinkOhms));
    addGroundBranch(a, kWiperNode, std::max(wiperResistance, kNodeLinkOhms));
  }
  if (loudnessBlend > 0.0f &&
      receiver.volumeControlLoudnessResistanceOhms > 0.0f) {
    a[kTapNode][kTapNode] +=
        loudnessBlend /
        std::max(receiver.volumeControlLoudnessResistanceOhms, 1e-9f);
  }
  if (loudnessBlend > 0.0f &&
      receiver.volumeControlLoudnessCapFarads > 0.0f) {
    float loudnessGc =
        loudnessBlend * receiver.volumeControlLoudnessCapFarads / dt;
    a[kTapNode][kTapNode] += loudnessGc;
    b[kTapNode] += loudnessGc * receiver.volumeControlTapVoltage;
  }
  addGroundBranch(a, kGridNode, receiver.gridLeakResistanceOhms);
  float couplingGc = receiver.couplingCapFarads / dt;
  a[kWiperNode][kWiperNode] += couplingGc;
  a[kGridNode][kGridNode] += couplingGc;
  a[kWiperNode][kGridNode] -= couplingGc;
  a[kGridNode][kWiperNode] -= couplingGc;
  b[kWiperNode] += couplingGc * receiver.couplingCapVoltage;
  b[kGridNode] -= couplingGc * receiver.couplingCapVoltage;
  float nodeVoltages[3] = {};
  if (!solveLinear3x3(a, b, nodeVoltages)) {
    reset(radio);
    return 0.0f;
  }
  receiver.gridVoltage = nodeVoltages[kGridNode];
  receiver.volumeControlTapVoltage = nodeVoltages[kTapNode];
  receiver.couplingCapVoltage =
      nodeVoltages[kWiperNode] - nodeVoltages[kGridNode];
  if (!std::isfinite(receiver.couplingCapVoltage) ||
      !std::isfinite(receiver.gridVoltage) ||
      !std::isfinite(receiver.volumeControlTapVoltage)) {
    reset(radio);
    return 0.0f;
  }
  float out = 0.0f;
  float plateCurrent = 0.0f;
  assert(receiver.tubeTriodeConnected &&
         "Receiver audio stage expects a triode model");
  out = processResistorLoadedTriodeStage(
      receiver.tubeBiasVolts + receiver.gridVoltage, 1.0f,
      receiver.tubePlateSupplyVolts, receiver.tubeQuiescentPlateVolts,
      receiver.tubeTriodeModel, receiver.tubeLoadResistanceOhms,
      receiver.tubePlateVoltage);
  plateCurrent = static_cast<float>(korenTriodePlateCurrent(
      receiver.tubeBiasVolts + receiver.gridVoltage, receiver.tubePlateVoltage,
      receiver.tubeTriodeModel));
  if (radio.calibration.enabled) {
    radio.calibration.maxReceiverPlateCurrentAmps =
        std::max(radio.calibration.maxReceiverPlateCurrentAmps, plateCurrent);
  }
  return out;
}

void RadioToneNode::init(Radio1938& radio, RadioInitContext&) {
  auto& tone = radio.tone;
  tone.presence.setPeaking(radio.sampleRate, tone.presenceHz, tone.presenceQ,
                           tone.presenceGainDb);
  tone.tiltLp.setLowpass(radio.sampleRate, tone.tiltSplitHz, kRadioBiquadQ);
}

void RadioToneNode::reset(Radio1938& radio) {
  auto& tone = radio.tone;
  tone.presence.reset();
  tone.tiltLp.reset();
}

float RadioToneNode::process(Radio1938& radio,
                             float y,
                             const RadioSampleContext&) {
  auto& tone = radio.tone;
  if (tone.presenceHz <= 0.0f) return y;
  return tone.presence.process(y);
}

void RadioPowerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& power = radio.power;
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
  power.sagAtk =
      std::exp(-1.0f / (radio.sampleRate * (power.sagAttackMs / 1000.0f)));
  power.sagRel =
      std::exp(-1.0f / (radio.sampleRate * (power.sagReleaseMs / 1000.0f)));
  if (power.postLpHz > 0.0f) {
    power.postLpf.setLowpass(radio.sampleRate, power.postLpHz, kRadioBiquadQ);
  } else {
    power.postLpf = Biquad{};
  }
  power.driverSourceResistanceOhms = parallelResistance(
      radio.receiverCircuit.tubeLoadResistanceOhms,
      radio.receiverCircuit.tubePlateResistanceOhms);
  power.driverSourceResistanceOhms =
      std::max(power.driverSourceResistanceOhms, 1.0f);
  TriodeOperatingPoint driverOp = solveTriodeOperatingPoint(
      power.tubePlateSupplyVolts, power.interstagePrimaryResistanceOhms,
      power.tubeBiasVolts, power.tubePlateDcVolts, power.tubePlateCurrentAmps,
      power.tubeMutualConductanceSiemens, power.tubeMu);
  power.tubeQuiescentPlateVolts = driverOp.plateVolts;
  power.tubeQuiescentPlateCurrentAmps = driverOp.plateCurrentAmps;
  power.tubePlateResistanceOhms = driverOp.rpOhms;
  power.tubeTriodeModel = driverOp.model;
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
  assert((std::fabs(power.outputTubeQuiescentPlateVolts -
                    power.outputTubePlateDcVolts) <=
          power.outputOperatingPointToleranceVolts) &&
         "6B4 output operating point diverged from the preset target");

  power.tubePlateVoltage = power.tubeQuiescentPlateVolts;
  power.outputGridAVolts = 0.0f;
  power.outputGridBVolts = 0.0f;
  power.interstageTransformer.configure(
      radio.sampleRate, power.interstagePrimaryLeakageInductanceHenries,
      power.interstageMagnetizingInductanceHenries,
      power.interstageTurnsRatioPrimaryToSecondary,
      power.interstagePrimaryResistanceOhms,
      power.interstagePrimaryCoreLossResistanceOhms,
      power.interstagePrimaryShuntCapFarads,
      power.interstageSecondaryLeakageInductanceHenries,
      power.interstageSecondaryResistanceOhms,
      power.interstageSecondaryShuntCapFarads,
      power.interstageIntegrationSubsteps);
  power.outputTransformer.configure(
      radio.sampleRate, power.outputTransformerPrimaryLeakageInductanceHenries,
      power.outputTransformerMagnetizingInductanceHenries,
      power.outputTransformerTurnsRatioPrimaryToSecondary,
      power.outputTransformerPrimaryResistanceOhms,
      power.outputTransformerPrimaryCoreLossResistanceOhms,
      power.outputTransformerPrimaryShuntCapFarads,
      power.outputTransformerSecondaryLeakageInductanceHenries,
      power.outputTransformerSecondaryResistanceOhms,
      power.outputTransformerSecondaryShuntCapFarads,
      power.outputTransformerIntegrationSubsteps);
  power.satOsLpIn = Biquad{};
  power.satOsLpOut = Biquad{};
}

void RadioPowerNode::reset(Radio1938& radio) {
  auto& power = radio.power;
  power.sagEnv = 0.0f;
  power.rectifierPhase = 0.0f;
  power.satOsPrev = 0.0f;
  power.gridCouplingCapVoltage = 0.0f;
  power.gridVoltage = 0.0f;
  power.tubePlateVoltage = power.tubeQuiescentPlateVolts;
  power.outputGridAVolts = 0.0f;
  power.outputGridBVolts = 0.0f;
  power.interstageTransformer.reset();
  power.outputTransformer.reset();
  power.postLpf.reset();
  power.satOsLpIn.reset();
  power.satOsLpOut.reset();
}

float RadioPowerNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext&) {
  auto& power = radio.power;
  auto& controlSense = radio.controlSense;
  if (radio.calibration.enabled) {
    radio.calibration.validationSampleCount++;
  }
  float powerT = clampf((power.sagEnv - power.sagStart) /
                            std::max(1e-6f, power.sagEnd - power.sagStart),
                        0.0f, 1.0f);
  float supplyScale = 1.0f - power.gainSagPerPower * powerT;
  float dt = 1.0f / std::max(radio.sampleRate, 1.0f);
  float previousCapVoltage = power.gridCouplingCapVoltage;
  power.gridVoltage = solveCapCoupledGridVoltage(
      y, previousCapVoltage, dt, power.gridCouplingCapFarads,
      power.driverSourceResistanceOhms, power.gridLeakResistanceOhms,
      power.tubeBiasVolts, power.tubeGridCurrentResistanceOhms);
  float seriesCurrent =
      (y - previousCapVoltage - power.gridVoltage) /
      (std::max(power.driverSourceResistanceOhms, 1.0f) +
       dt / std::max(power.gridCouplingCapFarads, 1e-12f));
  power.gridCouplingCapVoltage +=
      dt * (seriesCurrent / std::max(power.gridCouplingCapFarads, 1e-12f));
  float controlGridVolts = power.tubeBiasVolts + power.gridVoltage;
  if (radio.calibration.enabled && controlGridVolts > 0.0f) {
    radio.calibration.driverGridPositiveSamples++;
  }
  assert(power.tubeTriodeConnected &&
         "Interstage driver solve requires triode-connected 6F6 operation");
  float driverPlateQuiescent =
      std::max(power.tubeQuiescentPlateVolts * supplyScale, 1.0f);
  float driverQuiescentCurrent = static_cast<float>(korenTriodePlateCurrent(
      power.tubeBiasVolts, driverPlateQuiescent, power.tubeTriodeModel));
  struct SolvedTransformerDrive {
    float driveCurrent = 0.0f;
    CurrentDrivenTransformerSample sample{};
  };
  auto solvePrimaryVoltage = [&](auto&& projectTransformerSample,
                                 auto&& driveCurrentForVoltage,
                                 float initialGuess,
                                 float minPrimaryVoltage,
                                 float maxPrimaryVoltage) {
    auto evalResidual = [&](float primaryVoltageGuess,
                            CurrentDrivenTransformerSample& sampleOut,
                            float& driveCurrentOut) {
      driveCurrentOut = driveCurrentForVoltage(primaryVoltageGuess);
      sampleOut = projectTransformerSample(driveCurrentOut);
      return sampleOut.primaryVoltage - primaryVoltageGuess;
    };

    float bestGuess = clampf(initialGuess, minPrimaryVoltage, maxPrimaryVoltage);
    float bestDriveCurrent = 0.0f;
    CurrentDrivenTransformerSample bestSample{};
    float bestResidual =
        evalResidual(bestGuess, bestSample, bestDriveCurrent);

    CurrentDrivenTransformerSample lowSample{};
    CurrentDrivenTransformerSample highSample{};
    float lowDriveCurrent = 0.0f;
    float highDriveCurrent = 0.0f;
    float lowResidual =
        evalResidual(minPrimaryVoltage, lowSample, lowDriveCurrent);
    float highResidual =
        evalResidual(maxPrimaryVoltage, highSample, highDriveCurrent);

    if (std::fabs(lowResidual) < std::fabs(bestResidual)) {
      bestGuess = minPrimaryVoltage;
      bestResidual = lowResidual;
      bestDriveCurrent = lowDriveCurrent;
      bestSample = lowSample;
    }
    if (std::fabs(highResidual) < std::fabs(bestResidual)) {
      bestGuess = maxPrimaryVoltage;
      bestResidual = highResidual;
      bestDriveCurrent = highDriveCurrent;
      bestSample = highSample;
    }

    if (lowResidual * highResidual <= 0.0f) {
      float low = minPrimaryVoltage;
      float high = maxPrimaryVoltage;
      for (int i = 0; i < 8; ++i) {
        float mid = 0.5f * (low + high);
        CurrentDrivenTransformerSample midSample{};
        float midDriveCurrent = 0.0f;
        float midResidual = evalResidual(mid, midSample, midDriveCurrent);
        if (std::fabs(midResidual) < std::fabs(bestResidual)) {
          bestGuess = mid;
          bestResidual = midResidual;
          bestDriveCurrent = midDriveCurrent;
          bestSample = midSample;
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
    } else {
      float guess = bestGuess;
      for (int i = 0; i < 6; ++i) {
        CurrentDrivenTransformerSample predicted{};
        float driveCurrent = 0.0f;
        float residual = evalResidual(guess, predicted, driveCurrent);
        if (std::fabs(residual) < std::fabs(bestResidual)) {
          bestGuess = guess;
          bestResidual = residual;
          bestDriveCurrent = driveCurrent;
          bestSample = predicted;
        }
        guess = 0.5f * (guess + predicted.primaryVoltage);
      }
    }

    return SolvedTransformerDrive{bestDriveCurrent, bestSample};
  };
  auto interstageSolved = solveDriverInterstageNoCap(
      power.interstageTransformer, controlGridVolts, driverPlateQuiescent,
      driverQuiescentCurrent, power.tubeTriodeModel, 0.0f,
      power.outputTubeBiasVolts,
      power.outputGridLeakResistanceOhms,
      power.outputGridCurrentResistanceOhms);
  const auto& interstageSample = interstageSolved.transformer;
  power.interstageTransformer.primaryCurrent = interstageSample.primaryCurrent;
  power.interstageTransformer.secondaryCurrent =
      interstageSample.secondaryCurrent;
  power.interstageTransformer.primaryVoltage = interstageSample.primaryVoltage;
  power.interstageTransformer.secondaryVoltage =
      interstageSample.secondaryVoltage;
  if (radio.calibration.enabled) {
    float interstageSecondaryAbs = std::fabs(interstageSample.secondaryVoltage);
    radio.calibration.interstageSecondaryPeakVolts = std::max(
        radio.calibration.interstageSecondaryPeakVolts, interstageSecondaryAbs);
    radio.calibration.interstageSecondarySumSq +=
        static_cast<double>(interstageSample.secondaryVoltage) *
        static_cast<double>(interstageSample.secondaryVoltage);
  }
  power.tubePlateVoltage =
      driverPlateQuiescent - interstageSample.primaryVoltage;
  power.outputGridAVolts = 0.5f * interstageSample.secondaryVoltage;
  power.outputGridBVolts = -0.5f * interstageSample.secondaryVoltage;
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
      std::max(power.outputTubeQuiescentPlateVolts * supplyScale, 1.0f);
  float outputPrimaryMaxSwing =
      2.5f * std::max(outputPlateQuiescent, 1.0f);
  const float outputPrimaryLoadResistance = 0.0f;
  auto outputSolved = solvePrimaryVoltage(
      [&](float driveCurrentAmps) {
        return power.outputTransformer.project(
            driveCurrentAmps, power.outputLoadResistanceOhms,
            outputPrimaryLoadResistance);
      },
      [&](float primaryVoltageGuess) {
        float plateA = outputPlateQuiescent - 0.5f * primaryVoltageGuess;
        float plateB = outputPlateQuiescent + 0.5f * primaryVoltageGuess;
        float plateCurrentA = static_cast<float>(korenTriodePlateCurrent(
            power.outputTubeBiasVolts + power.outputGridAVolts, plateA,
            power.outputTubeTriodeModel));
        float plateCurrentB = static_cast<float>(korenTriodePlateCurrent(
            power.outputTubeBiasVolts + power.outputGridBVolts, plateB,
            power.outputTubeTriodeModel));
        return 0.5f * (plateCurrentA - plateCurrentB);
      },
      power.outputTransformer.primaryVoltage,
      -outputPrimaryMaxSwing, outputPrimaryMaxSwing);
  auto outputSample = power.outputTransformer.process(
      outputSolved.driveCurrent, power.outputLoadResistanceOhms,
      outputPrimaryLoadResistance);
  float actualDriverCurrent = interstageSolved.driverPlateCurrentAbs;
  float outputPlateA = outputPlateQuiescent - 0.5f * outputSample.primaryVoltage;
  float outputPlateB = outputPlateQuiescent + 0.5f * outputSample.primaryVoltage;
  float actualPlateCurrentA = static_cast<float>(korenTriodePlateCurrent(
      power.outputTubeBiasVolts + power.outputGridAVolts, outputPlateA,
      power.outputTubeTriodeModel));
  float actualPlateCurrentB = static_cast<float>(korenTriodePlateCurrent(
      power.outputTubeBiasVolts + power.outputGridBVolts, outputPlateB,
      power.outputTubeTriodeModel));
  y = outputSample.secondaryVoltage;
  if (power.postLpHz > 0.0f) {
    y = power.postLpf.process(y);
  }
  if (radio.calibration.enabled) {
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
                std::max(quiescentSupplyCurrent, 1e-6f));
  if (load > power.sagEnv) {
    power.sagEnv = power.sagAtk * power.sagEnv + (1.0f - power.sagAtk) * load;
  } else {
    power.sagEnv = power.sagRel * power.sagEnv + (1.0f - power.sagRel) * load;
  }
  controlSense.powerSagSense = power.sagEnv;
  return y;
}

void RadioInterferenceDerivedNode::init(Radio1938& radio, RadioInitContext&) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseDerived = radio.noiseDerived;
  if (radio.noiseWeight <= 0.0f) {
    noiseDerived.baseNoiseAmp = 0.0f;
    noiseDerived.baseCrackleAmp = 0.0f;
    noiseDerived.baseHumAmp = 0.0f;
    noiseDerived.crackleRate = 0.0f;
    return;
  }

  float scale = radio.noiseWeight / noiseConfig.noiseWeightRef;
  noiseDerived.baseNoiseAmp = radio.noiseWeight;
  noiseDerived.baseCrackleAmp = noiseConfig.crackleAmpScale * scale;
  noiseDerived.baseHumAmp = noiseConfig.humAmpScale * scale;
  noiseDerived.crackleRate = noiseConfig.crackleRateScale * scale;
}

void RadioInterferenceDerivedNode::reset(Radio1938&) {}

void RadioInterferenceDerivedNode::update(Radio1938& radio,
                                          RadioSampleContext& ctx) {
  auto& noiseDerived = radio.noiseDerived;
  ctx.derived.demodIfNoiseAmp =
      noiseDerived.baseNoiseAmp * radio.globals.ifNoiseMix;
  ctx.derived.noiseAmp = 0.0f;
  ctx.derived.crackleAmp = 0.0f;
  ctx.derived.crackleRate = 0.0f;
  ctx.derived.humAmp = 0.0f;
  ctx.derived.humToneEnabled = false;
}

void RadioNoiseNode::init(Radio1938& radio, RadioInitContext& initCtx) {
  auto& noiseConfig = radio.noiseConfig;
  auto& noiseRuntime = radio.noiseRuntime;
  noiseRuntime.hum.setFs(radio.sampleRate, initCtx.tunedBw);
  noiseRuntime.hum.humHz = noiseConfig.humHzDefault;
}

void RadioNoiseNode::reset(Radio1938& radio) { radio.noiseRuntime.hum.reset(); }

float RadioNoiseNode::process(Radio1938& radio,
                              float y,
                              const RadioSampleContext& ctx) {
  NoiseInput noiseIn{};
  noiseIn.programSample = y;
  noiseIn.noiseAmp = ctx.derived.noiseAmp;
  noiseIn.crackleAmp = ctx.derived.crackleAmp;
  noiseIn.crackleRate = ctx.derived.crackleRate;
  noiseIn.humAmp = ctx.derived.humAmp;
  noiseIn.humToneEnabled = ctx.derived.humToneEnabled;
  return y + radio.noiseRuntime.hum.process(noiseIn);
}

void RadioSpeakerNode::init(Radio1938& radio, RadioInitContext&) {
  auto& speakerStage = radio.speakerStage;
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  speakerStage.osLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  speakerStage.osLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
  speakerStage.speaker.init(osFs);
  speakerStage.speaker.drive = speakerStage.drive;
}

void RadioSpeakerNode::reset(Radio1938& radio) {
  auto& speakerStage = radio.speakerStage;
  speakerStage.osPrev = 0.0f;
  speakerStage.osLpIn.reset();
  speakerStage.osLpOut.reset();
  speakerStage.speaker.reset();
}

float RadioSpeakerNode::process(Radio1938& radio,
                                float y,
                                const RadioSampleContext&) {
  auto& speakerStage = radio.speakerStage;
  speakerStage.speaker.drive = std::max(speakerStage.drive, 0.0f);
  y = processOversampled2x(y, speakerStage.osPrev, speakerStage.osLpIn,
                           speakerStage.osLpOut, [&](float v) {
                             bool clipped = false;
                             float out = speakerStage.speaker.process(v, clipped);
                             if (clipped) radio.diagnostics.markSpeakerClip();
                             return out;
                           });
  return y;
}

void RadioCabinetNode::init(Radio1938& radio, RadioInitContext&) {
  auto& cabinet = radio.cabinet;
  float panelHzDerived =
      cabinet.panelHz * (1.0f + 0.52f * cabinet.panelStiffnessTolerance);
  float chassisHzDerived =
      cabinet.chassisHz * (1.0f + 0.28f * cabinet.panelStiffnessTolerance -
                           0.18f * cabinet.baffleLeakTolerance);
  float cavityDipHzDerived =
      cabinet.cavityDipHz * (1.0f + 0.35f * cabinet.cavityTolerance);
  float rearDelayMsDerived =
      cabinet.rearDelayMs *
      (1.0f + 0.30f * cabinet.rearPathTolerance +
       0.12f * cabinet.baffleLeakTolerance);
  cabinet.rearMixApplied =
      cabinet.rearMix * (1.0f + 0.42f * cabinet.baffleLeakTolerance -
                         0.25f * cabinet.grilleClothTolerance);

  if (cabinet.panelHz > 0.0f && cabinet.panelQ > 0.0f) {
    cabinet.panel.setPeaking(radio.sampleRate, panelHzDerived, cabinet.panelQ,
                             cabinet.panelGainDb);
  } else {
    cabinet.panel = Biquad{};
  }
  if (cabinet.chassisHz > 0.0f && cabinet.chassisQ > 0.0f) {
    cabinet.chassis.setPeaking(radio.sampleRate, chassisHzDerived,
                               cabinet.chassisQ, cabinet.chassisGainDb);
  } else {
    cabinet.chassis = Biquad{};
  }
  if (cabinet.cavityDipHz > 0.0f && cabinet.cavityDipQ > 0.0f) {
    cabinet.cavityDip.setPeaking(radio.sampleRate, cavityDipHzDerived,
                                 cabinet.cavityDipQ, cabinet.cavityDipGainDb);
  } else {
    cabinet.cavityDip = Biquad{};
  }
  if (cabinet.grilleLpHz > 0.0f) {
    float grilleLpHzDerived =
        cabinet.grilleLpHz / (1.0f + 0.55f * cabinet.grilleClothTolerance);
    cabinet.grilleLp.setLowpass(radio.sampleRate, grilleLpHzDerived,
                                kRadioBiquadQ);
  } else {
    cabinet.grilleLp = Biquad{};
  }
  if (cabinet.rearMixApplied > 0.0f) {
    if (cabinet.rearHpHz > 0.0f) {
      cabinet.rearHp.setHighpass(radio.sampleRate, cabinet.rearHpHz,
                                 kRadioBiquadQ);
    } else {
      cabinet.rearHp = Biquad{};
    }
    if (cabinet.rearLpHz > 0.0f) {
      cabinet.rearLp.setLowpass(radio.sampleRate, cabinet.rearLpHz,
                                kRadioBiquadQ);
    } else {
      cabinet.rearLp = Biquad{};
    }
    int rearSamples =
        static_cast<int>(std::ceil(rearDelayMsDerived * 0.001f *
                                   radio.sampleRate)) +
        cabinet.bufferGuardSamples;
    if (rearSamples > 0) {
      cabinet.buf.assign(static_cast<size_t>(rearSamples), 0.0f);
    } else {
      cabinet.buf.clear();
    }
  } else {
    cabinet.rearHp = Biquad{};
    cabinet.rearLp = Biquad{};
    cabinet.buf.clear();
  }
  cabinet.clarifier1 = Biquad{};
  cabinet.clarifier2 = Biquad{};
  cabinet.clarifier3 = Biquad{};
  cabinet.index = 0;
}

void RadioCabinetNode::reset(Radio1938& radio) {
  auto& cabinet = radio.cabinet;
  cabinet.panel.reset();
  cabinet.chassis.reset();
  cabinet.cavityDip.reset();
  cabinet.grilleLp.reset();
  cabinet.rearHp.reset();
  cabinet.rearLp.reset();
  cabinet.clarifier1.reset();
  cabinet.clarifier2.reset();
  cabinet.clarifier3.reset();
  cabinet.index = 0;
  std::fill(cabinet.buf.begin(), cabinet.buf.end(), 0.0f);
}

float RadioCabinetNode::process(Radio1938& radio,
                                float y,
                                const RadioSampleContext&) {
  auto& cabinet = radio.cabinet;
  if (!cabinet.enabled) return y;

  float out = cabinet.panel.process(y);
  out = cabinet.chassis.process(out);
  out = cabinet.cavityDip.process(out);
  if (!cabinet.buf.empty()) {
    float rear = cabinet.buf[static_cast<size_t>(cabinet.index)];
    cabinet.buf[static_cast<size_t>(cabinet.index)] = y;
    cabinet.index = (cabinet.index + 1) % static_cast<int>(cabinet.buf.size());
    rear = cabinet.rearHp.process(rear);
    rear = cabinet.rearLp.process(rear);
    out -= rear * cabinet.rearMixApplied;
  }
  if (cabinet.grilleLpHz > 0.0f) {
    out = cabinet.grilleLp.process(out);
  }
  return out;
}

void RadioFinalLimiterNode::init(Radio1938& radio, RadioInitContext&) {
  auto& limiter = radio.finalLimiter;
  limiter.attackCoeff =
      std::exp(-1.0f / (radio.sampleRate * (limiter.attackMs / 1000.0f)));
  limiter.releaseCoeff =
      std::exp(-1.0f / (radio.sampleRate * (limiter.releaseMs / 1000.0f)));
  limiter.delaySamples =
      static_cast<int>(std::lround(radio.sampleRate *
                                   (limiter.lookaheadMs / 1000.0f)));
  limiter.delayBuf.assign(static_cast<size_t>(limiter.delaySamples), 0.0f);
  limiter.requiredGainBuf.assign(static_cast<size_t>(limiter.delaySamples), 1.0f);
  limiter.delayWriteIndex = 0;
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  limiter.osLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  limiter.osLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
}

void RadioFinalLimiterNode::reset(Radio1938& radio) {
  auto& limiter = radio.finalLimiter;
  limiter.gain = 1.0f;
  limiter.targetGain = 1.0f;
  limiter.osPrev = 0.0f;
  limiter.observedPeak = 0.0f;
  limiter.delayWriteIndex = 0;
  std::fill(limiter.delayBuf.begin(), limiter.delayBuf.end(), 0.0f);
  std::fill(limiter.requiredGainBuf.begin(), limiter.requiredGainBuf.end(), 1.0f);
  limiter.osLpIn.reset();
  limiter.osLpOut.reset();
}

float RadioFinalLimiterNode::process(Radio1938& radio,
                                     float y,
                                     const RadioSampleContext&) {
  auto& limiter = radio.finalLimiter;
  if (!limiter.enabled) return y;
  float limitedIn = y;

  float peak = 0.0f;
  float mid = 0.5f * (limiter.osPrev + limitedIn);
  float s0 = limiter.osLpIn.process(mid);
  s0 = limiter.osLpOut.process(s0);
  peak = std::max(peak, std::fabs(s0));

  float s1 = limiter.osLpIn.process(limitedIn);
  s1 = limiter.osLpOut.process(s1);
  peak = std::max(peak, std::fabs(s1));

  limiter.osPrev = limitedIn;
  limiter.observedPeak = peak;

  float requiredGain = 1.0f;
  if (peak > limiter.threshold && peak > 1e-9f) {
    requiredGain = limiter.threshold / peak;
  }

  float delayed = limitedIn;
  if (!limiter.delayBuf.empty()) {
    size_t writeIndex = static_cast<size_t>(limiter.delayWriteIndex);
    delayed = limiter.delayBuf[writeIndex];
    limiter.delayBuf[writeIndex] = limitedIn;
    limiter.requiredGainBuf[writeIndex] = requiredGain;
    limiter.delayWriteIndex =
        (limiter.delayWriteIndex + 1) % static_cast<int>(limiter.delayBuf.size());
    limiter.targetGain = 1.0f;
    for (float gainCandidate : limiter.requiredGainBuf) {
      limiter.targetGain = std::min(limiter.targetGain, gainCandidate);
    }
  } else {
    limiter.targetGain = requiredGain;
  }

  if (limiter.targetGain < limiter.gain) {
    limiter.gain = limiter.targetGain;
  } else {
    limiter.gain =
        limiter.releaseCoeff * limiter.gain +
        (1.0f - limiter.releaseCoeff) * limiter.targetGain;
  }

  float out = delayed * limiter.gain;
  float gainReduction = 1.0f - limiter.gain;
  float gainReductionDb = (limiter.gain < 0.999999f) ? -lin2db(limiter.gain) : 0.0f;

  radio.diagnostics.finalLimiterPeak =
      std::max(radio.diagnostics.finalLimiterPeak, peak);
  radio.diagnostics.finalLimiterGain =
      std::min(radio.diagnostics.finalLimiterGain, limiter.gain);
  radio.diagnostics.finalLimiterMaxGainReduction =
      std::max(radio.diagnostics.finalLimiterMaxGainReduction, gainReduction);
  radio.diagnostics.finalLimiterMaxGainReductionDb =
      std::max(radio.diagnostics.finalLimiterMaxGainReductionDb, gainReductionDb);
  radio.diagnostics.finalLimiterGainReductionSum += gainReduction;
  radio.diagnostics.finalLimiterGainReductionDbSum += gainReductionDb;
  radio.diagnostics.processedSamples++;
  if (limiter.gain < 0.999f) {
    radio.diagnostics.finalLimiterActive = true;
    radio.diagnostics.finalLimiterActiveSamples++;
  }

  if (radio.calibration.enabled) {
    if (std::fabs(limitedIn) > radio.globals.outputClipThreshold) {
      radio.calibration.preLimiterClipCount++;
    }
    if (std::fabs(out) > radio.globals.outputClipThreshold) {
      radio.calibration.postLimiterClipCount++;
    }
    radio.calibration.totalSamples++;
    radio.calibration.limiterGainReductionSum += gainReduction;
    radio.calibration.limiterGainReductionDbSum += gainReductionDb;
    if (limiter.gain < 0.999f) {
      radio.calibration.limiterActiveSamples++;
    }
    radio.calibration.limiterMaxGainReduction =
        std::max(radio.calibration.limiterMaxGainReduction, gainReduction);
    radio.calibration.limiterMaxGainReductionDb =
        std::max(radio.calibration.limiterMaxGainReductionDb, gainReductionDb);
  }

  return out;
}

void RadioOutputClipNode::init(Radio1938& radio, RadioInitContext&) {
  auto& output = radio.output;
  float osFs = radio.sampleRate * radio.globals.oversampleFactor;
  float osCut = radio.sampleRate * radio.globals.oversampleCutoffFraction;
  output.clipOsLpIn.setLowpass(osFs, osCut, kRadioBiquadQ);
  output.clipOsLpOut.setLowpass(osFs, osCut, kRadioBiquadQ);
}

void RadioOutputClipNode::reset(Radio1938& radio) {
  auto& output = radio.output;
  output.clipOsPrev = 0.0f;
  output.clipOsLpIn.reset();
  output.clipOsLpOut.reset();
}

float RadioOutputClipNode::process(Radio1938& radio,
                                   float y,
                                   const RadioSampleContext&) {
  auto& output = radio.output;
  return processOversampled2x(y, output.clipOsPrev, output.clipOsLpIn,
                              output.clipOsLpOut, [&](float v) {
                                float t = radio.globals.outputClipThreshold;
                                float av = std::fabs(v);
                                if (av > t) radio.diagnostics.markOutputClip();
                                float clipped = softClip(v, t);
                                return std::clamp(clipped, -t, t);
                              });
}

Radio1938::BlockStep* Radio1938::RadioExecutionGraph::findBlock(StageId id) {
  for (auto& step : blockSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

const Radio1938::BlockStep* Radio1938::RadioExecutionGraph::findBlock(
    StageId id) const {
  for (const auto& step : blockSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

Radio1938::SampleControlStep* Radio1938::RadioExecutionGraph::findSampleControl(
    StageId id) {
  for (auto& step : sampleControlSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

const Radio1938::SampleControlStep*
Radio1938::RadioExecutionGraph::findSampleControl(StageId id) const {
  for (const auto& step : sampleControlSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

Radio1938::ProgramPathStep* Radio1938::RadioExecutionGraph::findProgramPath(
    StageId id) {
  for (auto& step : programPathSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

const Radio1938::ProgramPathStep* Radio1938::RadioExecutionGraph::findProgramPath(
    StageId id) const {
  for (const auto& step : programPathSteps) {
    if (step.id == id) return &step;
  }
  return nullptr;
}

bool Radio1938::RadioExecutionGraph::isEnabled(StageId id) const {
  if (const auto* step = findBlock(id)) return step->enabled;
  if (const auto* step = findSampleControl(id)) return step->enabled;
  if (const auto* step = findProgramPath(id)) return step->enabled;
  return false;
}

void Radio1938::RadioExecutionGraph::setEnabled(StageId id, bool value) {
  if (auto* step = findBlock(id)) {
    step->enabled = value;
    return;
  }
  if (auto* step = findSampleControl(id)) {
    step->enabled = value;
    return;
  }
  if (auto* step = findProgramPath(id)) {
    step->enabled = value;
    return;
  }
}

void Radio1938::RadioLifecycle::configure(Radio1938& radio,
                                          RadioInitContext& initCtx) const {
  for (const auto& step : configureSteps) {
    if (!step.init) continue;
    step.init(radio, initCtx);
  }
}

void Radio1938::RadioLifecycle::allocate(Radio1938& radio,
                                         RadioInitContext& initCtx) const {
  for (const auto& step : allocateSteps) {
    if (!step.init) continue;
    step.init(radio, initCtx);
  }
}

void Radio1938::RadioLifecycle::initializeDependentState(
    Radio1938& radio,
    RadioInitContext& initCtx) const {
  for (const auto& step : initializeDependentStateSteps) {
    if (!step.init) continue;
    step.init(radio, initCtx);
  }
}

void Radio1938::RadioLifecycle::resetRuntime(Radio1938& radio) const {
  for (const auto& step : resetSteps) {
    if (!step.reset) continue;
    step.reset(radio);
  }
}

static void updateCalibrationSnapshot(Radio1938& radio);

template <size_t StepCount>
static RadioBlockControl runBlockPrepare(
    Radio1938& radio,
    const std::array<BlockStep, StepCount>& steps,
    uint32_t frames) {
  RadioBlockControl block{};
  for (const auto& step : steps) {
    if (!step.enabled || !step.prepare) continue;
    step.prepare(radio, block, frames);
  }
  return block;
}

template <size_t StepCount>
static void runSampleControl(
    Radio1938& radio,
    RadioSampleContext& ctx,
    const std::array<SampleControlStep, StepCount>& steps) {
  for (const auto& step : steps) {
    if (!step.enabled || !step.update) continue;
    step.update(radio, ctx);
  }
}

template <size_t StepCount>
static float runProgramPath(
    Radio1938& radio,
    float y,
    const RadioSampleContext& ctx,
    const std::array<ProgramPathStep, StepCount>& steps) {
  for (const auto& step : steps) {
    if (!step.enabled || !step.process) continue;
    float in = y;
    y = step.process(radio, y, ctx);
    updateStageCalibration(radio, step.id, in, y);
  }
  return y;
}

template <size_t StepCount>
static float runProgramPathFromIndex(
    Radio1938& radio,
    float y,
    const RadioSampleContext& ctx,
    const std::array<ProgramPathStep, StepCount>& steps,
    size_t startIndex) {
  for (size_t i = startIndex; i < StepCount; ++i) {
    const auto& step = steps[i];
    if (!step.enabled || !step.process) continue;
    float in = y;
    y = step.process(radio, y, ctx);
    updateStageCalibration(radio, step.id, in, y);
  }
  return y;
}

static constexpr size_t kInputProgramStartIndex = 0;
static constexpr size_t kMixerProgramStartIndex = 2;

template <typename InputSampleFn, typename OutputSampleFn>
static void processRadioFrames(Radio1938& radio,
                               uint32_t frames,
                               size_t programStartIndex,
                               InputSampleFn&& inputSample,
                               OutputSampleFn&& outputSample) {
  if (frames == 0 || radio.graph.bypass) return;

  radio.diagnostics.reset();
  RadioBlockControl block = runBlockPrepare(radio, radio.graph.blockSteps, frames);
  for (uint32_t frame = 0; frame < frames; ++frame) {
    RadioSampleContext ctx{};
    ctx.block = &block;
    runSampleControl(radio, ctx, radio.graph.sampleControlSteps);
    float x = inputSample(frame);
    float yPhysical = runProgramPathFromIndex(
        radio, x, ctx, radio.graph.programPathSteps, programStartIndex);
    float yDigital = scalePhysicalSpeakerVoltsToDigital(radio, yPhysical);
    if (radio.calibration.enabled) {
      radio.calibration.maxDigitalOutput =
          std::max(radio.calibration.maxDigitalOutput, std::fabs(yDigital));
    }
    outputSample(frame, yDigital);
  }

  if (radio.calibration.enabled) {
    updateCalibrationSnapshot(radio);
  }
}

static float sampleInterleavedToMono(const float* samples,
                                     uint32_t frame,
                                     int channels) {
  if (!samples) return 0.0f;
  if (channels <= 1) return samples[frame];
  float sum = 0.0f;
  size_t base = static_cast<size_t>(frame) * static_cast<size_t>(channels);
  for (int channel = 0; channel < channels; ++channel) {
    sum += samples[base + static_cast<size_t>(channel)];
  }
  return sum / static_cast<float>(channels);
}

static void writeMonoToInterleaved(float* samples,
                                   uint32_t frame,
                                   int channels,
                                   float y) {
  if (!samples) return;
  if (channels <= 1) {
    samples[frame] = y;
    return;
  }
  size_t base = static_cast<size_t>(frame) * static_cast<size_t>(channels);
  for (int channel = 0; channel < channels; ++channel) {
    samples[base + static_cast<size_t>(channel)] = y;
  }
}

static void applyPhilco37116Preset(Radio1938& radio) {
  radio.globals.ifNoiseMix = 0.22f;
  radio.globals.inputPad = 1.0f;
  radio.globals.enableAutoLevel = false;
  radio.globals.autoTargetDb = -21.0f;
  radio.globals.autoMaxBoostDb = 2.5f;

  radio.tuning.safeBwMinHz = 2400.0f;
  radio.tuning.safeBwMaxHz = 9000.0f;
  radio.tuning.preBwScale = 1.00f;
  radio.tuning.postBwScale = 1.00f;
  radio.tuning.smoothTau = 0.05f;
  radio.tuning.updateEps = 0.25f;
  radio.tuning.magneticTuningEnabled = true;
  radio.tuning.afcCaptureHz = 420.0f;
  radio.tuning.afcMaxCorrectionHz = 110.0f;
  radio.tuning.afcDeadband = 0.015f;
  radio.tuning.afcResponseMs = 240.0f;

  radio.frontEnd.inputHpHz = 115.0f;
  radio.frontEnd.rfGain = 1.0f;
  radio.frontEnd.avcGainDepth = 0.18f;
  radio.frontEnd.selectivityPeakHz = 0.0f;
  radio.frontEnd.selectivityPeakQ = 0.707f;
  radio.frontEnd.selectivityPeakGainDb = 0.0f;
  radio.frontEnd.antennaInductanceHenries = 0.011f;
  radio.frontEnd.antennaLoadResistanceOhms = 2200.0f;
  radio.frontEnd.rfInductanceHenries = 0.016f;
  radio.frontEnd.rfLoadResistanceOhms = 3300.0f;

  // Philco 37-116 uses a 6L7G mixer. RCA's 1935 release data gives about
  // 250 V plate, 160 V screen, grid-1 bias near -6 V, grid-3 bias near -20 V,
  // 25 V peak oscillator injection, about 3.5 mA plate current, and at least
  // 325 umho conversion conductance. The reduced-order mixer keeps the same
  // cross-term isolation but now biases the oscillator grid explicitly.
  radio.mixer.rfGridDriveVolts = 1.0f;
  radio.mixer.loGridDriveVolts = 18.0f;
  radio.mixer.loGridBiasVolts = -15.0f;
  radio.mixer.avcGridDriveVolts = 24.0f;
  radio.mixer.plateSupplyVolts = 250.0f;
  radio.mixer.plateDcVolts = 250.0f;
  radio.mixer.screenVolts = 160.0f;
  radio.mixer.biasVolts = -6.0f;
  radio.mixer.cutoffVolts = -45.0f;
  radio.mixer.plateCurrentAmps = 0.0035f;
  radio.mixer.mutualConductanceSiemens = 0.0011f;
  radio.mixer.acLoadResistanceOhms = 10000.0f;
  radio.mixer.plateKneeVolts = 22.0f;
  radio.mixer.gridSoftnessVolts = 2.0f;

  radio.ifStrip.enabled = true;
  radio.ifStrip.ifMinBwHz = 2400.0f;
  radio.ifStrip.stageGain = 1.0f;
  radio.ifStrip.avcGainDepth = 0.18f;
  radio.ifStrip.ifCenterHz = 470000.0f;
  radio.ifStrip.primaryInductanceHenries = 220e-6f;
  radio.ifStrip.secondaryInductanceHenries = 250e-6f;
  radio.ifStrip.secondaryLoadResistanceOhms = 680.0f;
  radio.ifStrip.interstageCouplingCoeff = 0.15f;
  radio.ifStrip.outputCouplingCoeff = 0.11f;

  radio.demod.am.audioDiodeDrop = 0.0100f;
  radio.demod.am.avcDiodeDrop = 0.0080f;
  radio.demod.am.audioJunctionSlopeVolts = 0.0045f;
  radio.demod.am.avcJunctionSlopeVolts = 0.0040f;
  // The 37-116 detector/control network uses 6J5/6H6-derived AVC and audio
  // functions rather than the earlier 116X diode/load values. This remains a
  // reduced-order detector, but the downstream control loading is now aligned
  // to the 37-116 audio chain instead of the 116X/77 input stage.
  radio.demod.am.detectorPlateCouplingCapFarads = 110e-12f;
  radio.demod.am.audioChargeResistanceOhms = 5100.0f;
  radio.demod.am.audioDischargeResistanceOhms =
      51000.0f + parallelResistance(330000.0f, 2000000.0f);
  radio.demod.am.avcChargeResistanceOhms = 1000000.0f;
  radio.demod.am.avcDischargeResistanceOhms =
      parallelResistance(1000000.0f, 1000000.0f);
  radio.demod.am.avcFilterCapFarads = 0.15e-6f;
  radio.demod.am.controlVoltageRef = 0.75f;
  radio.demod.am.senseLowHz = 0.0f;
  radio.demod.am.senseHighHz = 0.0f;
  radio.demod.am.afcSenseLpHz = 34.0f;

  radio.receiverCircuit.enabled = true;
  // Philco 37-116 control 33-5158 is 2 Mohm total with a 1 Mohm tap. The
  // surrounding RC here stays reduced-order, but the control geometry and
  // first-audio tube are now anchored to the 37-116 chassis rather than 116X.
  radio.receiverCircuit.volumeControlResistanceOhms = 2000000.0f;
  radio.receiverCircuit.volumeControlTapResistanceOhms = 1000000.0f;
  radio.receiverCircuit.volumeControlPosition = 1.0f;
  radio.receiverCircuit.volumeControlLoudnessResistanceOhms = 490000.0f;
  radio.receiverCircuit.volumeControlLoudnessCapFarads = 0.015e-6f;
  radio.receiverCircuit.couplingCapFarads = 0.01e-6f;
  radio.receiverCircuit.gridLeakResistanceOhms = 1000000.0f;
  // The 37-116 first audio stage is a 6J5G triode. A common 250 V / -8 V
  // operating point with about 9 mA plate current, rp about 7.7k, gm about
  // 2600 umho, and mu about 20 puts the plate near the roughly 90 V socket
  // reading shown in the service data with a ~15k plate load.
  radio.receiverCircuit.tubePlateSupplyVolts = 250.0f;
  radio.receiverCircuit.tubePlateDcVolts = 90.0f;
  radio.receiverCircuit.tubeScreenVolts = 0.0f;
  radio.receiverCircuit.tubeBiasVolts = -8.0f;
  radio.receiverCircuit.tubePlateCurrentAmps = 0.009f;
  radio.receiverCircuit.tubeMutualConductanceSiemens = 0.0026f;
  radio.receiverCircuit.tubeMu = 20.0f;
  radio.receiverCircuit.tubeTriodeConnected = true;
  radio.receiverCircuit.tubeLoadResistanceOhms = 15000.0f;
  radio.receiverCircuit.tubePlateKneeVolts = 16.0f;
  radio.receiverCircuit.tubeGridSoftnessVolts = 0.6f;

  radio.tone.presenceHz = 0.0f;
  radio.tone.presenceQ = 0.78f;
  radio.tone.presenceGainDb = 0.0f;
  radio.tone.tiltSplitHz = 0.0f;

  radio.power.sagStart = 0.06f;
  radio.power.sagEnd = 0.22f;
  radio.power.rippleDepth = 0.0f;
  radio.power.satDrive = 1.0f;
  radio.power.satMix = 0.0f;
  radio.power.sagAttackMs = 60.0f;
  radio.power.sagReleaseMs = 900.0f;
  radio.power.rectifierMinHz = 80.0f;
  radio.power.rippleSecondHarmonicMix = 0.0f;
  radio.power.gainSagPerPower = 0.015f;
  radio.power.rippleGainBase = 0.0f;
  radio.power.rippleGainDepth = 0.0f;
  radio.power.gainMin = 0.92f;
  radio.power.gainMax = 1.02f;
  radio.power.supplyDriveDepth = 0.0f;
  radio.power.supplyBiasDepth = 0.0f;
  radio.power.postLpHz = 0.0f;
  // The 37-116 driver is a triode-connected 6F6G feeding the 6B4G pair through
  // an interstage transformer. RC-13 gives 250 V plate, -20 V grid, about
  // 31 mA plate current, mu about 7, gm about 2700 umho, and 4000 ohm load
  // for single-ended class-A triode operation.
  radio.power.gridCouplingCapFarads = 0.05e-6f;
  radio.power.gridLeakResistanceOhms = 100000.0f;
  radio.power.tubePlateSupplyVolts = 250.0f;
  radio.power.tubeScreenVolts = 250.0f;
  radio.power.tubeBiasVolts = -20.0f;
  radio.power.tubePlateCurrentAmps = 0.031f;
  radio.power.tubeMutualConductanceSiemens = 0.0027f;
  radio.power.tubeMu = 7.0f;
  radio.power.tubeTriodeConnected = true;
  radio.power.tubeAcLoadResistanceOhms = 4000.0f;
  radio.power.tubePlateKneeVolts = 24.0f;
  radio.power.tubeGridSoftnessVolts = 0.8f;
  radio.power.tubeGridCurrentResistanceOhms = 1000.0f;
  radio.power.outputGridLeakResistanceOhms = 250000.0f;
  radio.power.outputGridCurrentResistanceOhms = 1200.0f;
  radio.power.interstagePrimaryLeakageInductanceHenries = 0.45f;
  radio.power.interstageMagnetizingInductanceHenries = 15.0f;
  radio.power.interstagePrimaryResistanceOhms = 430.0f;
  radio.power.tubePlateDcVolts =
      radio.power.tubePlateSupplyVolts -
      radio.power.tubePlateCurrentAmps *
          radio.power.interstagePrimaryResistanceOhms;
  radio.power.interstageTurnsRatioPrimaryToSecondary = 1.0f / 1.4f;
  radio.power.interstagePrimaryCoreLossResistanceOhms = 220000.0f;
  // The pF stray capacitances are ultrasonic details in the real chassis.
  // In this 48 kHz reduced-order audio model they destabilize the transformer
  // solve and collapse the audible output, so the preset leaves them out.
  radio.power.interstagePrimaryShuntCapFarads = 0.0f;
  radio.power.interstageSecondaryLeakageInductanceHenries = 0.040f;
  radio.power.interstageSecondaryResistanceOhms = 296.0f;
  radio.power.interstageSecondaryShuntCapFarads = 0.0f;
  radio.power.interstageIntegrationSubsteps = 8;
  // The 37-116 uses push-pull 6B4G output tubes. The 6B4G is the octal form of
  // the 2A3 family, so the reduced-order triode law keeps the same mu/gm class
  // and the 37-116's plate-to-plate load target.
  radio.power.outputTubePlateSupplyVolts = 325.0f;
  radio.power.outputTubeBiasVolts = -39.0f;
  radio.power.outputTubePlateCurrentAmps = 0.040f;
  radio.power.outputTubeMutualConductanceSiemens = 0.00525f;
  radio.power.outputTubeMu = 4.2f;
  radio.power.outputTubePlateToPlateLoadOhms = 3000.0f;
  radio.power.outputTubePlateKneeVolts = 18.0f;
  radio.power.outputTubeGridSoftnessVolts = 2.0f;
  radio.power.outputTransformerPrimaryLeakageInductanceHenries = 35e-3f;
  radio.power.outputTransformerMagnetizingInductanceHenries = 20.0f;
  radio.power.outputTransformerTurnsRatioPrimaryToSecondary =
      std::sqrt(3000.0f / 3.9f);
  radio.power.outputTransformerPrimaryResistanceOhms = 235.0f;
  radio.power.outputTubePlateDcVolts =
      radio.power.outputTubePlateSupplyVolts -
      radio.power.outputTubePlateCurrentAmps *
          (0.5f * radio.power.outputTransformerPrimaryResistanceOhms);
  radio.power.outputTransformerPrimaryCoreLossResistanceOhms = 90000.0f;
  radio.power.outputTransformerPrimaryShuntCapFarads = 0.0f;
  radio.power.outputTransformerSecondaryLeakageInductanceHenries = 60e-6f;
  radio.power.outputTransformerSecondaryResistanceOhms = 0.32f;
  radio.power.outputTransformerSecondaryShuntCapFarads = 0.0f;
  radio.power.outputTransformerIntegrationSubsteps = 8;
  radio.power.outputLoadResistanceOhms = 3.9f;
  radio.power.nominalOutputPowerWatts = 15.0f;
  assert(std::fabs(radio.power.tubePlateSupplyVolts -
                   radio.power.tubePlateCurrentAmps *
                       radio.power.interstagePrimaryResistanceOhms -
                   radio.power.tubePlateDcVolts) < 1.0f);
  assert(std::fabs(radio.power.outputTubePlateSupplyVolts -
                   radio.power.outputTubePlateCurrentAmps *
                       (0.5f * radio.power.outputTransformerPrimaryResistanceOhms) -
                   radio.power.outputTubePlateDcVolts) < 1.0f);
  radio.output.digitalReferenceSpeakerVoltsPeak = std::sqrt(
      2.0f * radio.power.nominalOutputPowerWatts *
      radio.power.outputLoadResistanceOhms);
}

static void applySetIdentity(Radio1938& radio) {
  (void)radio;
}

static void refreshIdentityDependentStages(Radio1938& radio) {
  applySetIdentity(radio);
  RadioInitContext initCtx{};
  RadioMixerNode::init(radio, initCtx);
  RadioMixerNode::reset(radio);
  RadioReceiverCircuitNode::init(radio, initCtx);
  RadioReceiverCircuitNode::reset(radio);
  RadioPowerNode::init(radio, initCtx);
  RadioPowerNode::reset(radio);
  if (radio.calibration.enabled) {
    radio.resetCalibration();
  }
}

std::string_view Radio1938::presetName(Preset preset) {
  switch (preset) {
    case Preset::Philco37116:
      return "philco_37_116";
  }
  return "philco_37_116";
}

Radio1938::Radio1938() { applyPreset(preset); }

std::string_view Radio1938::stageName(StageId id) {
  switch (id) {
    case StageId::Tuning:
      return "MagneticTuning";
    case StageId::Input:
      return "Input";
    case StageId::AVC:
      return "AVC";
    case StageId::AFC:
      return "AFC";
    case StageId::ControlBus:
      return "ControlBus";
    case StageId::InterferenceDerived:
      return "InterferenceDerived";
    case StageId::FrontEnd:
      return "RFFrontEnd";
    case StageId::Mixer:
      return "Mixer";
    case StageId::IFStrip:
      return "IFStrip";
    case StageId::Demod:
      return "Detector";
    case StageId::ReceiverCircuit:
      return "AudioStage";
    case StageId::Tone:
      return "Tone";
    case StageId::Power:
      return "Power";
    case StageId::Noise:
      return "Noise";
    case StageId::Speaker:
      return "Speaker";
    case StageId::Cabinet:
      return "Cabinet";
    case StageId::FinalLimiter:
      return "FinalLimiter";
    case StageId::OutputClip:
      return "OutputClip";
  }
  return "Unknown";
}

bool Radio1938::applyPreset(std::string_view presetNameValue) {
  if (presetNameValue == "philco_37_116") {
    applyPreset(Preset::Philco37116);
    return true;
  }
  if (presetNameValue == "philco_37_116x") {
    applyPreset(Preset::Philco37116);
    return true;
  }
  return false;
}

void Radio1938::applyPreset(Preset presetValue) {
  preset = presetValue;
  switch (presetValue) {
    case Preset::Philco37116:
      applyPhilco37116Preset(*this);
      break;
  }
  if (!initialized) return;
  init(channels, sampleRate, bwHz, noiseWeight);
}

void Radio1938::setIdentitySeed(uint32_t seed) {
  identity.seed = seed;
  if (!initialized) return;
  refreshIdentityDependentStages(*this);
}

void Radio1938::setCalibrationEnabled(bool enabled) {
  calibration.enabled = enabled;
  resetCalibration();
}

void Radio1938::resetCalibration() {
  calibration.reset();
}

void Radio1938::init(int ch, float sr, float bw, float noise) {
  channels = ch;
  sampleRate = sr;
  bwHz = bw;
  noiseWeight = noise;
  applySetIdentity(*this);
  RadioInitContext initCtx{};
  lifecycle.configure(*this, initCtx);
  lifecycle.allocate(*this, initCtx);
  lifecycle.initializeDependentState(*this, initCtx);
  initialized = true;
  if (calibration.enabled) {
    resetCalibration();
  }
  reset();
}

void Radio1938::reset() {
  diagnostics.reset();
  iqInput.resetRuntime();
  sourceFrame.resetRuntime();
  lifecycle.resetRuntime(*this);
  if (calibration.enabled) {
    resetCalibration();
  }
}

void Radio1938::processIfReal(float* samples, uint32_t frames) {
  if (!samples || frames == 0) return;
  processRadioFrames(
      *this, frames, kInputProgramStartIndex,
      [&](uint32_t frame) {
        float x = sampleInterleavedToMono(samples, frame, channels);
        sourceFrame.setRealRf(x);
        return x;
      },
      [&](uint32_t frame, float y) {
        writeMonoToInterleaved(samples, frame, channels, y);
      });
}

void Radio1938::processAmAudio(const float* audioSamples,
                               float* outSamples,
                               uint32_t frames,
                               float receivedCarrierRmsVolts,
                               float modulationIndex) {
  if (!audioSamples || !outSamples || frames == 0) return;
  std::vector<float> rfScratch(frames);
  float safeSampleRate = std::max(sampleRate, 1.0f);
  float carrierHz =
      std::clamp(ifStrip.sourceCarrierHz, 1000.0f, safeSampleRate * 0.45f);
  float carrierStep = kRadioTwoPi * (carrierHz / safeSampleRate);
  float carrierPeak =
      std::sqrt(2.0f) * std::max(receivedCarrierRmsVolts, 0.0f);
  float phase = iqInput.iqPhase;
  for (uint32_t frame = 0; frame < frames; ++frame) {
    float envelopeFactor =
        std::max(0.0f, 1.0f + modulationIndex * audioSamples[frame]);
    float envelope = carrierPeak * envelopeFactor;
    rfScratch[frame] = envelope * std::cos(phase);
    phase += carrierStep;
    if (phase >= kRadioTwoPi) phase -= kRadioTwoPi;
  }
  iqInput.iqPhase = phase;
  processIfReal(rfScratch.data(), frames);
  std::copy(rfScratch.begin(), rfScratch.end(), outSamples);
}

void Radio1938::processIqBaseband(const float* iqInterleaved,
                                  float* outSamples,
                                  uint32_t frames) {
  if (!iqInterleaved || !outSamples || frames == 0) return;
  processRadioFrames(
      *this, frames, kMixerProgramStartIndex,
      [&](uint32_t frame) {
        size_t base = static_cast<size_t>(frame) * 2u;
        float i = iqInterleaved[base];
        float q = iqInterleaved[base + 1u];
        sourceFrame.setComplexEnvelope(i, q);
        return i;
      },
      [&](uint32_t frame, float y) {
        writeMonoToInterleaved(outSamples, frame, channels, y);
      });
}
