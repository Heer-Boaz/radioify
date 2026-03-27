#ifndef RADIOIFY_AUDIOFILTER_MATH_RADIO_LINEAR_SOLVERS_H
#define RADIOIFY_AUDIOFILTER_MATH_RADIO_LINEAR_SOLVERS_H

bool solveLinear3x3(float a[3][3], float b[3], float x[3]);
bool solveLinear4x4(float a[4][4], float b[4], float x[4]);
bool solveLinear3x3Direct(float a00,
                          float a01,
                          float a02,
                          float a10,
                          float a11,
                          float a12,
                          float a20,
                          float a21,
                          float a22,
                          const float rhs[3],
                          float x[3]);

#endif
