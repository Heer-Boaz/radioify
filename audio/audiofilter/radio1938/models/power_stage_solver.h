#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_SOLVER_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_SOLVER_H

#include "../../../radio.h"
#include "speaker_sim.h"
#include "../../models/transformer_models.h"

struct DriverInterstageCenterTappedResult {
  float driverPlateCurrentAbs = 0.0f;
  float primaryCurrent = 0.0f;
  float primaryVoltage = 0.0f;
  float secondaryACurrent = 0.0f;
  float secondaryAVoltage = 0.0f;
  float secondaryBCurrent = 0.0f;
  float secondaryBVoltage = 0.0f;
  int substeps = 0;
  int totalIterations = 0;
  int maxIterations = 0;
  uint64_t driverEvalCount = 0;
  uint64_t driverEvalTimeNs = 0;
  uint32_t adaptiveValidationAttempts = 0;
  uint32_t adaptiveAcceptedDownshifts = 0;
  float adaptiveBoundaryErrorVolts = 0.0f;
  int suggestedSubsteps = 0;
  int suggestedValidationCountdown = 0;
};

struct SpeakerElectricalLinearization {
  SecondaryNortonLoad load;
  float electricalCoeff = 0.0f;
  float mechanicalCoeff = 0.0f;
  float determinant = 0.0f;
  float rhsElectrical = 0.0f;
  float rhsMechanical = 0.0f;
  float dt = 0.0f;
  float forceFactorBl = 0.0f;
};

struct OutputStageSubstepResult {
  CurrentDrivenTransformer transformer;
  float averagePrimaryVoltage = 0.0f;
  float averageSecondaryVoltage = 0.0f;
  float averagePlateCurrentA = 0.0f;
  float averagePlateCurrentB = 0.0f;
  int transformerSubsteps = 0;
  int totalNewtonIterations = 0;
  int maxNewtonIterations = 0;
};

struct OutputPrimarySolveResult {
  float primaryVoltage = 0.0f;
  float driveCurrent = 0.0f;
  float plateCurrentA = 0.0f;
  float plateCurrentB = 0.0f;
  int iterations = 0;
};

OutputPrimarySolveResult solveOutputPrimaryAffine(
    const AffineTransformerProjection& projection,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float gridA,
    float gridB,
    float initialPrimaryVoltage);

SpeakerElectricalLinearization linearizeSpeakerElectricalLoad(
    const SpeakerSim& speaker,
    float nominalLoadOhms,
    float dt);

void commitSpeakerElectricalLoad(SpeakerSim& speaker,
                                 const SpeakerElectricalLinearization& l,
                                 float appliedVolts);

OutputStageSubstepResult runOutputStageSubsteps(
    CurrentDrivenTransformer transformer,
    SpeakerSim& speaker,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float outputPrimaryLoadResistance);

float estimateOutputStageNominalPowerWatts(
    const Radio1938::PowerNodeState& power);

DriverInterstageCenterTappedResult solveDriverInterstageCenterTappedNoCap(
    const CurrentDrivenTransformer& transformer,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts,
    float driverPlateQuiescent,
    float driverQuiescentCurrent,
    bool measureDriverEvalTime);

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_SOLVER_H
