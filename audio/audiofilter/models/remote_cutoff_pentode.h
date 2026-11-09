#ifndef RADIOIFY_AUDIOFILTER_MODELS_REMOTE_CUTOFF_PENTODE_H
#define RADIOIFY_AUDIOFILTER_MODELS_REMOTE_CUTOFF_PENTODE_H

// RCA 78 / 6K7 "super-control" (remote-cutoff) RF pentode. Its tapered grid
// makes transconductance fall roughly exponentially with negative bias, which
// is what lets AVC control gain over tens of dB without cutting the tube off.
// RCA notes that the 78 and 6K7 characteristics are identical.
//
// Stage gain of a pentode into a tuned load is gm * Z, with plate resistance
// far above the load, so an AVC-controlled stage scales with gm alone.
// Screen voltage moves the cutoff almost proportionally (RCA's 75/100/125 V
// curves coincide within the reading accuracy after scaling grid volts by
// 100/Ec2), so other screen voltages use that scaling.

// Transconductance in siemens at Ec2 = 100 V (Eb 250 V).
double remoteCutoffTransconductance(double gridVolts);

// Absolute gm of a stage at its own screen voltage, by the same scaling.
double remoteCutoffStageTransconductance(double biasVolts, double avcVolts,
                                         double screenVolts);

// gm(bias - avc) / gm(bias) for a stage at the given screen voltage.
double remoteCutoffRelativeGain(double biasVolts, double avcVolts, double screenVolts);

#endif  // RADIOIFY_AUDIOFILTER_MODELS_REMOTE_CUTOFF_PENTODE_H
