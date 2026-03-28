#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_SOLVER_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_SOLVER_H

#include "../../../radio.h"
#include "../../models/transformer_models.h"

struct DriverInterstageCenterTappedResult {
  float driverPlateCurrentAbs = 0.0f;
  float primaryCurrent = 0.0f;
  float primaryVoltage = 0.0f;
  float secondaryACurrent = 0.0f;
  float secondaryAVoltage = 0.0f;
  float secondaryBCurrent = 0.0f;
  float secondaryBVoltage = 0.0f;
};

float solveOutputPrimaryVoltageAffine(
    const AffineTransformerProjection& projection,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float gridA,
    float gridB,
    float initialPrimaryVoltage);

float estimateOutputStageNominalPowerWatts(
    const Radio1938::PowerNodeState& power);

DriverInterstageCenterTappedResult solveDriverInterstageCenterTappedNoCap(
    const CurrentDrivenTransformer& transformer,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts,
    float driverPlateQuiescent,
    float driverQuiescentCurrent);

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_SOLVER_H
