#include "../../../radio.h"
#include "../../math/signal_math.h"

#include <algorithm>
#include <cmath>

void RadioAFCNode::init(Radio1938&, RadioInitContext&) {}

void RadioAFCNode::reset(Radio1938&) {}

void RadioAFCNode::run(Radio1938& radio, RadioSampleContext&) {
  auto& tuning = radio.tuning;
  const auto& controlSense = radio.controlSense;
  if (!tuning.magneticTuningEnabled || tuning.afcMaxCorrectionHz <= 0.0f ||
      tuning.afcResponseMs <= 0.0f) {
    tuning.afcCorrectionHz = 0.0f;
    return;
  }

  float error = controlSense.tuningErrorSense;
  error = std::copysign(std::max(std::fabs(error) - tuning.afcDeadband, 0.0f), error);
  float captureGate =
      (tuning.afcCaptureHz > 0.0f &&
       std::fabs(tuning.tuneOffsetHz) <= tuning.afcCaptureHz)
          ? 1.0f
          : 0.0f;
  // Magnetic tuning has its own IF detector. Delayed AVC can be zero on a
  // perfectly usable carrier and must not switch this discriminator off.
  float signalT = clampf(
      controlSense.tuningSignalVolts / std::max(tuning.afcSignalReferenceVolts, 1e-6f),
      0.0f, 1.0f);
  const float afcTarget = clampf(-error * tuning.afcLoopGain, -1.0f, 1.0f) *
                          tuning.afcMaxCorrectionHz * captureGate * signalT;
  tuning.afcCorrectionHz +=
      tuning.afcTick * (afcTarget - tuning.afcCorrectionHz);
  tuning.afcCorrectionHz =
      clampf(tuning.afcCorrectionHz, -tuning.afcMaxCorrectionHz,
             tuning.afcMaxCorrectionHz);
}
