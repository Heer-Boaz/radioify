#ifndef RADIOIFY_AUDIOFILTER_MODELS_TUBE_MODELS_H
#define RADIOIFY_AUDIOFILTER_MODELS_TUBE_MODELS_H

#include <cmath>
#include <cstddef>
#include <vector>

struct KorenTriodeModel {
  double mu = 1.0;
  double ex = 1.5;
  double kg1 = 1.0;
  double kp = 1.0;
  double kvb = 1.0;
};

struct KorenTriodeLut {
  float vgkMin = 0.0f;
  float vgkMax = 0.0f;
  float vpkMin = 0.0f;
  float vpkMax = 0.0f;
  float vgkInvStep = 0.0f;
  float vpkInvStep = 0.0f;
  int vgkBins = 0;
  int vpkBins = 0;
  std::vector<float> currentAmps;
  std::vector<float> conductanceSiemens;

  bool valid() const {
    return vgkBins >= 2 && vpkBins >= 2 &&
           currentAmps.size() ==
               static_cast<size_t>(vgkBins * vpkBins) &&
           conductanceSiemens.size() ==
               static_cast<size_t>(vgkBins * vpkBins);
  }
};

struct TriodeOperatingPoint {
  float plateVolts = 0.0f;
  float plateCurrentAmps = 0.0f;
  float rpOhms = 0.0f;
  KorenTriodeModel model{};
};

struct PentodeOperatingPoint {
  float plateVolts = 0.0f;
  float plateCurrentAmps = 0.0f;
  float rpOhms = 0.0f;
};

struct KorenTriodePlateEval {
  double currentAmps = 0.0;
  double conductanceSiemens = 0.0;
};

struct FixedPlatePentodeEvaluator {
  float cutoffVolts = 0.0f;
  float gridSoftnessVolts = 0.0f;
  float exponent = 1.0f;
  float currentScale = 0.0f;

  float eval(float gridVolts) const;
  float evalDerivative(float gridVolts) const;
};

TriodeOperatingPoint solveTriodeOperatingPoint(
    float plateSupplyVolts,
    float loadResistanceOhms,
    float biasVolts,
    float targetPlateVolts,
    float targetPlateCurrentAmps,
    float targetMutualConductanceSiemens,
    float mu);

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
    float& fittedCutoffVolts);

float solveCapCoupledGridVoltage(float sourceVolts,
                                 float previousCapVoltage,
                                 float dt,
                                 float couplingCapFarads,
                                 float sourceResistanceOhms,
                                 float gridLeakResistanceOhms,
                                 float biasVolts,
                                 float gridCurrentResistanceOhms);

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
                                     float& plateVoltageState);

float processResistorLoadedTriodeStage(float gridVolts,
                                       float supplyScale,
                                       float plateSupplyVolts,
                                       float quiescentPlateVolts,
                                       const KorenTriodeModel& model,
                                       const KorenTriodeLut* lut,
                                       float loadResistanceOhms,
                                       float& plateVoltageState);

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
    float gridSoftnessVolts);

KorenTriodePlateEval evaluateKorenTriodePlateRuntime(
    double vgk,
    double vpk,
    const KorenTriodeModel& model,
    const KorenTriodeLut& lut);

void configureRuntimeTriodeLut(KorenTriodeLut& lut,
                               const KorenTriodeModel& model,
                               float biasVolts,
                               float plateSupplyVolts,
                               float extraNegativeGridHeadroomVolts,
                               float positiveGridHeadroomVolts);

#endif  // RADIOIFY_AUDIOFILTER_MODELS_TUBE_MODELS_H
