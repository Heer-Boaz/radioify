#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_INTERNAL_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_INTERNAL_H

#include "power_stage_solver.h"

#include <chrono>

namespace power_stage_internal {

inline uint64_t monotonicNowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

struct OutputTubePairEval {
  float plateCurrentA = 0.0f;
  float plateCurrentB = 0.0f;
  float driveCurrent = 0.0f;
  float driveSlope = 0.0f;
};

struct TriodeLutView {
  const float* currentAmps = nullptr;
  const float* conductanceSiemens = nullptr;
  float vgkMin = 0.0f;
  float vgkMax = 0.0f;
  float vpkMin = 0.0f;
  float vpkMax = 0.0f;
  float vgkInvStep = 0.0f;
  float vpkInvStep = 0.0f;
  int vgkBins = 0;
  int vpkBins = 0;
  bool valid = false;
};

struct TubeGridBranchEval {
  float current = 0.0f;
  float slope = 0.0f;
};

TubeGridBranchEval evaluateTubeGridBranch(float acGridVolts,
                                          float biasVolts,
                                          float gridLeakConductance,
                                          float gridCurrentConductance);

TriodeLutView makeTriodeLutView(const KorenTriodeLut& lut);

KorenTriodePlateEval evaluateTriodePlateFast(float vgk,
                                             float vpk,
                                             const KorenTriodeModel& model,
                                             const KorenTriodeLut& lut,
                                             const TriodeLutView& view);

OutputTubePairEval evaluateOutputTubePair(
    const Radio1938::PowerNodeState& power,
    float outputPlateQuiescent,
    float gridA,
    float gridB,
    float primaryVoltage);

int chooseAdaptiveInterstageSubsteps(
    const CurrentDrivenTransformer& transformer,
    const Radio1938::PowerNodeState& power,
    float controlGridVolts);

int nextLowerAdaptiveSubsteps(int currentSubsteps);

float estimateInterstageBoundaryErrorVolts(
    const DriverInterstageCenterTappedResult& coarse,
    const DriverInterstageCenterTappedResult& fine);

}  // namespace power_stage_internal

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_INTERNAL_H
