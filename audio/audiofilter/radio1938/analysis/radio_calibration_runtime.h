#ifndef RADIOIFY_AUDIOFILTER_RADIO1938_ANALYSIS_RADIO_CALIBRATION_RUNTIME_H
#define RADIOIFY_AUDIOFILTER_RADIO1938_ANALYSIS_RADIO_CALIBRATION_RUNTIME_H

#include "../../../radio.h"

void updatePassCalibration(Radio1938& radio, PassId id, float in, float out);
void updateCalibrationSnapshot(Radio1938& radio);
void beginClickTraceSample(Radio1938& radio);
void recordClickTracePassOutput(Radio1938& radio, PassId id, float out);
void finishClickTraceSample(Radio1938& radio, float out);

#endif  // RADIOIFY_AUDIOFILTER_RADIO1938_ANALYSIS_RADIO_CALIBRATION_RUNTIME_H
