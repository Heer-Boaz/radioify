#include "resonant_networks.h"

#include "../math/signal_math.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>

void SeriesRlcBandpass::setModel(float newFs,
                                 float newInductanceHenries,
                                 float newCapacitanceFarads,
                                 float newSeriesResistanceOhms,
                                 float newOutputResistanceOhms,
                                 int newIntegrationSubsteps) {
  assert(std::isfinite(newFs) && newFs > 0.0f);
  assert(std::isfinite(newInductanceHenries) && newInductanceHenries > 0.0f);
  assert(std::isfinite(newCapacitanceFarads) && newCapacitanceFarads > 0.0f);
  assert(std::isfinite(newSeriesResistanceOhms) &&
         newSeriesResistanceOhms >= 0.0f);
  assert(std::isfinite(newOutputResistanceOhms) &&
         newOutputResistanceOhms >= 0.0f);
  assert(newIntegrationSubsteps > 0);
  fs = newFs;
  inductanceHenries = newInductanceHenries;
  capacitanceFarads = newCapacitanceFarads;
  seriesResistanceOhms = newSeriesResistanceOhms;
  outputResistanceOhms = newOutputResistanceOhms;
  integrationSubsteps = newIntegrationSubsteps;
  dtSub = 1.0f / (fs * static_cast<float>(integrationSubsteps));
  float subA00 = 1.0f - dtSub * (seriesResistanceOhms / inductanceHenries);
  float subA01 = -dtSub / inductanceHenries;
  float subB0 = dtSub / inductanceHenries;
  float capStep = dtSub / capacitanceFarads;
  float subA10 = capStep * subA00;
  float subA11 = 1.0f + capStep * subA01;
  float subB1 = capStep * subB0;
  macroA00 = 1.0f;
  macroA01 = 0.0f;
  macroA10 = 0.0f;
  macroA11 = 1.0f;
  macroB0 = 0.0f;
  macroB1 = 0.0f;
  for (int step = 0; step < integrationSubsteps; ++step) {
    float nextA00 = subA00 * macroA00 + subA01 * macroA10;
    float nextA01 = subA00 * macroA01 + subA01 * macroA11;
    float nextA10 = subA10 * macroA00 + subA11 * macroA10;
    float nextA11 = subA10 * macroA01 + subA11 * macroA11;
    float nextB0 = subA00 * macroB0 + subA01 * macroB1 + subB0;
    float nextB1 = subA10 * macroB0 + subA11 * macroB1 + subB1;
    macroA00 = nextA00;
    macroA01 = nextA01;
    macroA10 = nextA10;
    macroA11 = nextA11;
    macroB0 = nextB0;
    macroB1 = nextB1;
  }
}

void SeriesRlcBandpass::clearState() {
  inductorCurrent = 0.0f;
  capacitorVoltage = 0.0f;
}

float SeriesRlcBandpass::step(float vin) {
  float nextInductorCurrent =
      macroA00 * inductorCurrent + macroA01 * capacitorVoltage + macroB0 * vin;
  float nextCapacitorVoltage =
      macroA10 * inductorCurrent + macroA11 * capacitorVoltage + macroB1 * vin;
  inductorCurrent = nextInductorCurrent;
  capacitorVoltage = nextCapacitorVoltage;
  assert(std::isfinite(inductorCurrent) && std::isfinite(capacitorVoltage));
  return inductorCurrent * outputResistanceOhms;
}

void CoupledTunedTransformer::setModel(float newFs,
                                       float newPrimaryInductanceHenries,
                                       float newPrimaryCapacitanceFarads,
                                       float newPrimaryResistanceOhms,
                                       float newSecondaryInductanceHenries,
                                       float newSecondaryCapacitanceFarads,
                                       float newSecondaryResistanceOhms,
                                       float newCouplingCoeff,
                                       float newOutputResistanceOhms,
                                       int newIntegrationSubsteps) {
  assert(std::isfinite(newFs) && newFs > 0.0f);
  assert(std::isfinite(newPrimaryInductanceHenries) &&
         newPrimaryInductanceHenries > 0.0f);
  assert(std::isfinite(newPrimaryCapacitanceFarads) &&
         newPrimaryCapacitanceFarads > 0.0f);
  assert(std::isfinite(newPrimaryResistanceOhms) &&
         newPrimaryResistanceOhms >= 0.0f);
  assert(std::isfinite(newSecondaryInductanceHenries) &&
         newSecondaryInductanceHenries > 0.0f);
  assert(std::isfinite(newSecondaryCapacitanceFarads) &&
         newSecondaryCapacitanceFarads > 0.0f);
  assert(std::isfinite(newSecondaryResistanceOhms) &&
         newSecondaryResistanceOhms >= 0.0f);
  assert(std::isfinite(newCouplingCoeff) && std::fabs(newCouplingCoeff) < 1.0f);
  assert(std::isfinite(newOutputResistanceOhms) &&
         newOutputResistanceOhms >= 0.0f);
  assert(newIntegrationSubsteps > 0);
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
  mutualInductance =
      couplingCoeff *
      std::sqrt(primaryInductanceHenries * secondaryInductanceHenries);
  float determinant = primaryInductanceHenries * secondaryInductanceHenries -
                      mutualInductance * mutualInductance;
  assert(determinant > 0.0f);
  determinantInv = 1.0f / determinant;
  dtSub = 1.0f / (fs * static_cast<float>(integrationSubsteps));
  float k = dtSub * determinantInv;
  float subA00 =
      1.0f - k * secondaryInductanceHenries * primaryResistanceOhms;
  float subA01 = k * mutualInductance * secondaryResistanceOhms;
  float subA02 = -k * secondaryInductanceHenries;
  float subA03 = k * mutualInductance;
  float subA10 = k * mutualInductance * primaryResistanceOhms;
  float subA11 =
      1.0f - k * primaryInductanceHenries * secondaryResistanceOhms;
  float subA12 = k * mutualInductance;
  float subA13 = -k * primaryInductanceHenries;
  float subB0 = k * secondaryInductanceHenries;
  float subB1 = -k * mutualInductance;
  float primaryCapStep = dtSub / primaryCapacitanceFarads;
  float secondaryCapStep = dtSub / secondaryCapacitanceFarads;
  std::array<float, 16> subA{
      subA00, subA01, subA02, subA03, subA10, subA11, subA12, subA13,
      primaryCapStep * subA00, primaryCapStep * subA01,
      1.0f + primaryCapStep * subA02, primaryCapStep * subA03,
      secondaryCapStep * subA10, secondaryCapStep * subA11,
      secondaryCapStep * subA12, 1.0f + secondaryCapStep * subA13,
  };
  std::array<float, 4> subB{
      subB0,
      subB1,
      primaryCapStep * subB0,
      secondaryCapStep * subB1,
  };
  macroA = {};
  macroA[0] = 1.0f;
  macroA[5] = 1.0f;
  macroA[10] = 1.0f;
  macroA[15] = 1.0f;
  macroB = {};
  for (int step = 0; step < integrationSubsteps; ++step) {
    std::array<float, 16> nextA{};
    std::array<float, 4> nextB{};
    for (int row = 0; row < 4; ++row) {
      for (int col = 0; col < 4; ++col) {
        float sum = 0.0f;
        for (int kIdx = 0; kIdx < 4; ++kIdx) {
          sum += subA[row * 4 + kIdx] * macroA[kIdx * 4 + col];
        }
        nextA[row * 4 + col] = sum;
      }
      float sum = subB[row];
      for (int kIdx = 0; kIdx < 4; ++kIdx) {
        sum += subA[row * 4 + kIdx] * macroB[kIdx];
      }
      nextB[row] = sum;
    }
    macroA = nextA;
    macroB = nextB;
  }
}

void CoupledTunedTransformer::clearState() {
  primaryCurrent = 0.0f;
  primaryCapVoltage = 0.0f;
  secondaryCurrent = 0.0f;
  secondaryCapVoltage = 0.0f;
}

float CoupledTunedTransformer::step(float vin) {
  float nextPrimaryCurrent = macroA[0] * primaryCurrent +
                             macroA[1] * secondaryCurrent +
                             macroA[2] * primaryCapVoltage +
                             macroA[3] * secondaryCapVoltage + macroB[0] * vin;
  float nextSecondaryCurrent = macroA[4] * primaryCurrent +
                               macroA[5] * secondaryCurrent +
                               macroA[6] * primaryCapVoltage +
                               macroA[7] * secondaryCapVoltage + macroB[1] * vin;
  float nextPrimaryCapVoltage = macroA[8] * primaryCurrent +
                                macroA[9] * secondaryCurrent +
                                macroA[10] * primaryCapVoltage +
                                macroA[11] * secondaryCapVoltage + macroB[2] * vin;
  float nextSecondaryCapVoltage = macroA[12] * primaryCurrent +
                                  macroA[13] * secondaryCurrent +
                                  macroA[14] * primaryCapVoltage +
                                  macroA[15] * secondaryCapVoltage + macroB[3] * vin;
  primaryCurrent = nextPrimaryCurrent;
  secondaryCurrent = nextSecondaryCurrent;
  primaryCapVoltage = nextPrimaryCapVoltage;
  secondaryCapVoltage = nextSecondaryCapVoltage;
  assert(std::isfinite(primaryCurrent) && std::isfinite(primaryCapVoltage) &&
         std::isfinite(secondaryCurrent) &&
         std::isfinite(secondaryCapVoltage));

  return secondaryCurrent * outputResistanceOhms;
}
