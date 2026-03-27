#include "../radio.h"
#include "math/radio_math.h"

#include <algorithm>
#include <cmath>

void RadioAFCNode::update(Radio1938& radio, RadioSampleContext&) {
  auto& tuning = radio.tuning;
  const auto& controlSense = radio.controlSense;
  if (!tuning.magneticTuningEnabled || tuning.afcMaxCorrectionHz <= 0.0f ||
      tuning.afcResponseMs <= 0.0f) {
    tuning.afcCorrectionHz = 0.0f;
    return;
  }

  float rate = std::max(radio.sampleRate, 1.0f);
  float afcSeconds = tuning.afcResponseMs * 0.001f;
  float afcTick = 1.0f - std::exp(-1.0f / (rate * afcSeconds));
  float error = controlSense.tuningErrorSense;
  if (std::fabs(error) < tuning.afcDeadband) error = 0.0f;
  float captureT =
      1.0f - clampf(std::fabs(tuning.tuneOffsetHz) /
                        std::max(tuning.afcCaptureHz, 1e-6f),
                    0.0f, 1.0f);
  float signalT =
      clampf(controlSense.controlVoltageSense / 0.85f, 0.0f, 1.0f);
  float afcTarget =
      -error * tuning.afcMaxCorrectionHz * captureT * signalT;
  tuning.afcCorrectionHz += afcTick * (afcTarget - tuning.afcCorrectionHz);
  tuning.afcCorrectionHz =
      clampf(tuning.afcCorrectionHz, -tuning.afcMaxCorrectionHz,
             tuning.afcMaxCorrectionHz);
}
