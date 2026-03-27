#ifndef RADIOIFY_AUDIOFILTER_PHYSICS_RADIO_TRANSFORMERS_H
#define RADIOIFY_AUDIOFILTER_PHYSICS_RADIO_TRANSFORMERS_H

#include "../../radio.h"

#include <array>

struct AffineTransformerProjection {
  CurrentDrivenTransformerSample base{};
  CurrentDrivenTransformerSample slope{};
};

struct FixedLoadAffineTransformerProjection {
  std::array<float, 16> stateA{};
  CurrentDrivenTransformerSample slope{};
};

struct DriverInterstageCenterTappedResult {
  float driverPlateCurrentAbs = 0.0f;
  float primaryCurrent = 0.0f;
  float primaryVoltage = 0.0f;
  float secondaryACurrent = 0.0f;
  float secondaryAVoltage = 0.0f;
  float secondaryBCurrent = 0.0f;
  float secondaryBVoltage = 0.0f;
};

CurrentDrivenTransformerSample evalFixedLoadAffineBase(
    const std::array<float, 16>& stateA,
    const CurrentDrivenTransformer& t);

FixedLoadAffineTransformerProjection buildFixedLoadAffineProjection(
    const CurrentDrivenTransformer& t,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms);

AffineTransformerProjection buildAffineProjection(
    const CurrentDrivenTransformer& t,
    float secondaryLoadResistanceOhms,
    float primaryLoadResistanceOhms);

CurrentDrivenTransformerSample evalAffineProjection(
    const AffineTransformerProjection& p,
    float driveCurrent);

float solveOutputPrimaryVoltageAffine(
    const AffineTransformerProjection& proj,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float gridA,
    float gridB,
    float initialVp);

float estimateOutputStageNominalPowerWatts(
    const Radio1938::PowerNodeState& power);

DriverInterstageCenterTappedResult solveDriverInterstageCenterTappedNoCap(
    const CurrentDrivenTransformer& t,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts,
    float driverPlateQuiescent,
    float driverQuiescentCurrent);

#endif
