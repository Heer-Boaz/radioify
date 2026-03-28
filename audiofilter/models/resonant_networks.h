#ifndef RADIOIFY_AUDIOFILTER_MODELS_RESONANT_NETWORKS_H
#define RADIOIFY_AUDIOFILTER_MODELS_RESONANT_NETWORKS_H

#include <array>

struct SeriesRlcBandpass {
  float fs = 0.0f;
  float inductanceHenries = 0.0f;
  float capacitanceFarads = 0.0f;
  float seriesResistanceOhms = 0.0f;
  float outputResistanceOhms = 0.0f;
  int integrationSubsteps = 1;
  float dtSub = 0.0f;
  float macroA00 = 1.0f;
  float macroA01 = 0.0f;
  float macroA10 = 0.0f;
  float macroA11 = 1.0f;
  float macroB0 = 0.0f;
  float macroB1 = 0.0f;
  float inductorCurrent = 0.0f;
  float capacitorVoltage = 0.0f;

  void setModel(float newFs,
                float newInductanceHenries,
                float newCapacitanceFarads,
                float newSeriesResistanceOhms,
                float newOutputResistanceOhms,
                int newIntegrationSubsteps = 1);
  void clearState();
  float step(float vin);
};

struct CoupledTunedTransformer {
  float fs = 0.0f;
  float primaryInductanceHenries = 0.0f;
  float primaryCapacitanceFarads = 0.0f;
  float primaryResistanceOhms = 0.0f;
  float secondaryInductanceHenries = 0.0f;
  float secondaryCapacitanceFarads = 0.0f;
  float secondaryResistanceOhms = 0.0f;
  float couplingCoeff = 0.0f;
  float outputResistanceOhms = 0.0f;
  int integrationSubsteps = 1;
  float dtSub = 0.0f;
  float mutualInductance = 0.0f;
  float determinantInv = 0.0f;
  std::array<float, 16> macroA{};
  std::array<float, 4> macroB{};
  float primaryCurrent = 0.0f;
  float primaryCapVoltage = 0.0f;
  float secondaryCurrent = 0.0f;
  float secondaryCapVoltage = 0.0f;

  void setModel(float newFs,
                float newPrimaryInductanceHenries,
                float newPrimaryCapacitanceFarads,
                float newPrimaryResistanceOhms,
                float newSecondaryInductanceHenries,
                float newSecondaryCapacitanceFarads,
                float newSecondaryResistanceOhms,
                float newCouplingCoeff,
                float newOutputResistanceOhms,
                int newIntegrationSubsteps = 1);
  void clearState();
  float step(float vin);
};

#endif  // RADIOIFY_AUDIOFILTER_MODELS_RESONANT_NETWORKS_H
