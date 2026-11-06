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
  uint32_t adaptiveValidationAttempts = 0;
  uint32_t adaptiveAcceptedDownshifts = 0;
  float adaptiveBoundaryErrorVolts = 0.0f;
  int suggestedSubsteps = 0;
  int suggestedValidationCountdown = 0;
};

struct SpeakerElectricalLinearization {
  SecondaryNortonLoad load;
  double electricalCoeff = 0.0;
  double electricalCoupling = 0.0;
  double mechanicalCoeff = 0.0;
  double determinant = 0.0;
  double rhsElectrical = 0.0;
  double rhsMechanical = 0.0;
  double dt = 0.0;
  double forceFactorBl = 0.0;
};

struct SpeakerMotion {
  double currentAmps = 0.0;
  double velocityMetersPerSecond = 0.0;
};

struct OutputStageSubstepResult {
  CurrentDrivenTransformer transformer;
  float averagePrimaryVoltage = 0.0f;
  float averageSecondaryVoltage = 0.0f;
  float averageAcousticDriveVolts = 0.0f;
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
    double dt,
    const SpeakerMotion& estimate);

// Constant-terminal-voltage bench using the same nonlinear solve as playback.
void advanceVoltageDrivenSpeaker(SpeakerSim& speaker, float volts, float dt);

void configureSpeakerElectromechanics(SpeakerSim& speaker,
                                      float sampleRate,
                                      float nominalLoadOhms);

void resetSpeakerElectromechanics(SpeakerSim& speaker);

void commitSpeakerElectricalLoad(SpeakerSim& speaker,
                                 const SpeakerElectricalLinearization& l,
                                 float appliedVolts);

OutputStageSubstepResult runOutputStageSubsteps(
    CurrentDrivenTransformer transformer,
    SpeakerSim& speaker,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float outputPrimaryLoadResistance);

OutputStageSubstepResult runSingleEndedOutputStageSubsteps(
    CurrentDrivenTransformer transformer,
    SpeakerSim& speaker,
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float outputGridVolts,
    float outputPrimaryLoadResistance);

float estimateOutputStageNominalPowerWatts(
    const Radio1938::PowerNodeState& power);

float estimateSingleEndedOutputStageNominalPowerWatts(
    const Radio1938::PowerNodeState& power);

DriverInterstageCenterTappedResult solveDriverInterstageCenterTappedNoCap(
    const CurrentDrivenTransformer& transformer,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts,
    float driverPlateQuiescent,
    float driverQuiescentCurrent);

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_SOLVER_H
