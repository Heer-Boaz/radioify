#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_INTERNAL_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_INTERNAL_H

#include <chrono>

#include "power_stage_solver.h"

namespace power_stage_internal {

inline uint64_t monotonicNowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

struct OutputTubePairEval {
  double plateCurrentA = 0.0;
  double plateCurrentB = 0.0;
  double driveCurrent = 0.0;
  double driveSlope = 0.0;
};

struct TriodeLutView {
  const float* currentAmps = nullptr;
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
  double current = 0.0;
  double slope = 0.0;
};

TubeGridBranchEval evaluateTubeGridBranch(double acGridVolts, double biasVolts,
                                          double gridLeakConductance,
                                          double gridCurrentConductance);

TriodeLutView makeTriodeLutView(const KorenTriodeLut& lut);

KorenTriodePlateEval evaluateTriodePlateFast(double vgk, double vpk,
                                             const KorenTriodeModel& model,
                                             const KorenTriodeLut& lut,
                                             const TriodeLutView& view);

OutputTubePairEval evaluateOutputTubePair(const Radio1938::PowerNodeState& power,
                                          const TriodeLutView& outputTriodeLut,
                                          float outputPlateQuiescent, float gridA,
                                          float gridB, double primaryVoltage);

int chooseAdaptiveInterstageSubsteps(const CurrentDrivenTransformer& transformer,
                                     const Radio1938::PowerNodeState& power,
                                     float controlGridVolts);

int nextLowerAdaptiveSubsteps(int currentSubsteps);

float estimateInterstageBoundaryErrorVolts(
    const DriverInterstageCenterTappedResult& coarse,
    const DriverInterstageCenterTappedResult& fine);

}  // namespace power_stage_internal

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_MODELS_POWER_STAGE_INTERNAL_H
