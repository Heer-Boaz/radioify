#ifndef RADIOIFY_AUDIOFILTER_PHYSICS_RADIO_TUBE_MODELS_H
#define RADIOIFY_AUDIOFILTER_PHYSICS_RADIO_TUBE_MODELS_H

#include "../../radio.h"

#include <cmath>

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

#endif
