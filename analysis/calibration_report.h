#ifndef CALIBRATION_REPORT_H
#define CALIBRATION_REPORT_H

#include <cstdint>
#include <string>

struct Radio1938;

void printCalibrationReport(const Radio1938& radio1938, const std::string& label);

void printNodeStepSummaryHeader();

void printNodeStepSummaryLine(const std::string& disabledNode,
                             const Radio1938& result,
                             const Radio1938* baseline);

#endif  // CALIBRATION_REPORT_H
