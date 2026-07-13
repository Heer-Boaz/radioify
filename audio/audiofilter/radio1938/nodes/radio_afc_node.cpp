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
  if (std::fabs(error) < tuning.afcDeadband) error = 0.0f;
  float captureGate =
      (tuning.afcCaptureHz > 0.0f &&
       std::fabs(tuning.tuneOffsetHz) <= tuning.afcCaptureHz)
          ? 1.0f
          : 0.0f;
  float signalT =
      clampf(controlSense.controlVoltageSense / 0.85f, 0.0f, 1.0f);
  float afcTarget =
      -error * tuning.afcMaxCorrectionHz * captureGate * signalT;
  tuning.afcCorrectionHz +=
      tuning.afcTick * (afcTarget - tuning.afcCorrectionHz);
  tuning.afcCorrectionHz =
      clampf(tuning.afcCorrectionHz, -tuning.afcMaxCorrectionHz,
             tuning.afcMaxCorrectionHz);
}
