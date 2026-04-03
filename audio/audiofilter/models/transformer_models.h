#ifndef RADIOIFY_AUDIOFILTER_MODELS_TRANSFORMER_MODELS_H
#define RADIOIFY_AUDIOFILTER_MODELS_TRANSFORMER_MODELS_H

#include <array>

struct CurrentDrivenTransformerSample {
  float primaryVoltage = 0.0f;
  float secondaryVoltage = 0.0f;
  float primaryCurrent = 0.0f;
  float secondaryCurrent = 0.0f;
};

struct SecondaryNortonLoad {
  float conductanceSiemens = 0.0f;
  float currentAmps = 0.0f;
};

struct CurrentDrivenTransformer {
  float fs = 0.0f;
  float primaryLeakageInductanceHenries = 0.0f;
  float magnetizingInductanceHenries = 0.0f;
  float turnsRatioPrimaryToSecondary = 1.0f;
  float primaryResistanceOhms = 0.0f;
  float primaryCoreLossResistanceOhms = 0.0f;
  float primaryShuntCapFarads = 0.0f;
  float secondaryLeakageInductanceHenries = 0.0f;
  float secondaryResistanceOhms = 0.0f;
  float secondaryShuntCapFarads = 0.0f;
  int integrationSubsteps = 1;
  float dtSub = 0.0f;
  float cachedTurns = 0.0f;
  float cachedPrimaryInductance = 0.0f;
  float cachedSecondaryInductance = 0.0f;
  float cachedMutualInductance = 0.0f;
  float primaryCurrent = 0.0f;
  float secondaryCurrent = 0.0f;
  float primaryVoltage = 0.0f;
  float secondaryVoltage = 0.0f;

  void setModel(float newFs,
                float newPrimaryLeakageInductanceHenries,
                float newMagnetizingInductanceHenries,
                float newTurnsRatioPrimaryToSecondary,
                float newPrimaryResistanceOhms,
                float newPrimaryCoreLossResistanceOhms,
                float newPrimaryShuntCapFarads,
                float newSecondaryLeakageInductanceHenries,
                float newSecondaryResistanceOhms,
                float newSecondaryShuntCapFarads,
                int newIntegrationSubsteps = 1);
  void clearState();
  CurrentDrivenTransformerSample projectStep(
      float primaryDriveCurrentAmps,
      float secondaryLoadResistanceOhms,
      float primaryLoadResistanceOhms = 0.0f) const;
  CurrentDrivenTransformerSample projectStep(
      float primaryDriveCurrentAmps,
      const SecondaryNortonLoad& secondaryLoad,
      float primaryLoadResistanceOhms = 0.0f) const;
  CurrentDrivenTransformerSample step(float primaryDriveCurrentAmps,
                                      float secondaryLoadResistanceOhms,
                                      float primaryLoadResistanceOhms = 0.0f);
  CurrentDrivenTransformerSample step(float primaryDriveCurrentAmps,
                                      const SecondaryNortonLoad& secondaryLoad,
                                      float primaryLoadResistanceOhms = 0.0f);
};

struct AffineTransformerProjection {
  CurrentDrivenTransformerSample base{};
  CurrentDrivenTransformerSample slope{};
};

struct FixedLoadAffineTransformerProjection {
  std::array<float, 16> stateA{};
  CurrentDrivenTransformerSample slope{};
};

CurrentDrivenTransformerSample evalFixedLoadAffineBase(
    const std::array<float, 16>& stateA,
    const CurrentDrivenTransformer& transformer);

FixedLoadAffineTransformerProjection buildFixedLoadAffineProjection(
    const CurrentDrivenTransformer& transformer,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms);

AffineTransformerProjection buildAffineProjection(
    const CurrentDrivenTransformer& transformer,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms);

AffineTransformerProjection buildAffineProjection(
    const CurrentDrivenTransformer& transformer,
    const SecondaryNortonLoad& secondaryLoad,
    float primaryLoadResistanceOhms);

CurrentDrivenTransformerSample evalAffineProjection(
    const AffineTransformerProjection& projection,
    float driveCurrent);

#endif  // RADIOIFY_AUDIOFILTER_MODELS_TRANSFORMER_MODELS_H
