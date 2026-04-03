#ifndef RADIOIFY_AUDIOFILTER_MATH_LINEAR_SOLVERS_H
#define RADIOIFY_AUDIOFILTER_MATH_LINEAR_SOLVERS_H

#include <algorithm>
#include <cmath>

inline bool solveLinear3x3(float a[3][3], float b[3], float x[3]) {
  for (int pivot = 0; pivot < 3; ++pivot) {
    int pivotRow = pivot;
    float pivotAbs = std::fabs(a[pivot][pivot]);
    for (int row = pivot + 1; row < 3; ++row) {
      float candidateAbs = std::fabs(a[row][pivot]);
      if (candidateAbs > pivotAbs) {
        pivotAbs = candidateAbs;
        pivotRow = row;
      }
    }
    if (pivotAbs < 1e-12f) return false;
    if (pivotRow != pivot) {
      for (int col = pivot; col < 3; ++col) {
        std::swap(a[pivot][col], a[pivotRow][col]);
      }
      std::swap(b[pivot], b[pivotRow]);
    }
    float invPivot = 1.0f / a[pivot][pivot];
    for (int row = pivot + 1; row < 3; ++row) {
      float scale = a[row][pivot] * invPivot;
      if (std::fabs(scale) < 1e-12f) continue;
      for (int col = pivot; col < 3; ++col) {
        a[row][col] -= scale * a[pivot][col];
      }
      b[row] -= scale * b[pivot];
    }
  }
  for (int row = 2; row >= 0; --row) {
    float sum = b[row];
    for (int col = row + 1; col < 3; ++col) {
      sum -= a[row][col] * x[col];
    }
    if (std::fabs(a[row][row]) < 1e-12f) return false;
    x[row] = sum / a[row][row];
  }
  return true;
}

inline bool solveLinear4x4(float a[4][4], float b[4], float x[4]) {
  for (int pivot = 0; pivot < 4; ++pivot) {
    int pivotRow = pivot;
    float pivotAbs = std::fabs(a[pivot][pivot]);
    for (int row = pivot + 1; row < 4; ++row) {
      float candidateAbs = std::fabs(a[row][pivot]);
      if (candidateAbs > pivotAbs) {
        pivotAbs = candidateAbs;
        pivotRow = row;
      }
    }
    if (pivotAbs < 1e-12f) return false;
    if (pivotRow != pivot) {
      for (int col = pivot; col < 4; ++col) {
        std::swap(a[pivot][col], a[pivotRow][col]);
      }
      std::swap(b[pivot], b[pivotRow]);
    }
    float invPivot = 1.0f / a[pivot][pivot];
    for (int row = pivot + 1; row < 4; ++row) {
      float scale = a[row][pivot] * invPivot;
      if (std::fabs(scale) < 1e-12f) continue;
      for (int col = pivot; col < 4; ++col) {
        a[row][col] -= scale * a[pivot][col];
      }
      b[row] -= scale * b[pivot];
    }
  }
  for (int row = 3; row >= 0; --row) {
    float sum = b[row];
    for (int col = row + 1; col < 4; ++col) {
      sum -= a[row][col] * x[col];
    }
    if (std::fabs(a[row][row]) < 1e-12f) return false;
    x[row] = sum / a[row][row];
  }
  return true;
}

inline bool solveLinear3x3Direct(float a00,
                                 float a01,
                                 float a02,
                                 float a10,
                                 float a11,
                                 float a12,
                                 float a20,
                                 float a21,
                                 float a22,
                                 const float rhs[3],
                                 float x[3]) {
  float c00 = a11 * a22 - a12 * a21;
  float c01 = a12 * a20 - a10 * a22;
  float c02 = a10 * a21 - a11 * a20;
  float det = a00 * c00 + a01 * c01 + a02 * c02;
  if (!std::isfinite(det) || std::fabs(det) < 1e-12f) {
    return false;
  }
  float invDet = 1.0f / det;

  x[0] = (rhs[0] * c00 +
          rhs[1] * (a02 * a21 - a01 * a22) +
          rhs[2] * (a01 * a12 - a02 * a11)) *
         invDet;
  x[1] = (rhs[0] * c01 +
          rhs[1] * (a00 * a22 - a02 * a20) +
          rhs[2] * (a02 * a10 - a00 * a12)) *
         invDet;
  x[2] = (rhs[0] * c02 +
          rhs[1] * (a01 * a20 - a00 * a21) +
          rhs[2] * (a00 * a11 - a01 * a10)) *
         invDet;
  return std::isfinite(x[0]) && std::isfinite(x[1]) && std::isfinite(x[2]);
}

#endif  // RADIOIFY_AUDIOFILTER_MATH_LINEAR_SOLVERS_H
