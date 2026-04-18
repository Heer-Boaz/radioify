#include "asciiart.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <immintrin.h>
#include <mfapi.h>
#include <mfobjects.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

#include "asciiart_image_decode_wic.h"
#include "asciiart_layout.h"

#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#define RESTRICT __restrict
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#define RESTRICT __restrict__
#endif

namespace {
constexpr uint32_t kBrailleBase = 0x2800;

#define RADIOIFY_ASCII_BOOL(name, value) constexpr bool name = value;
#define RADIOIFY_ASCII_FLOAT(name, value) constexpr float name = value;
#define RADIOIFY_ASCII_COUNT(name, value) constexpr int name = value;
#define RADIOIFY_ASCII_LUMA_U8(name, value) constexpr int name = value;
#define RADIOIFY_ASCII_SIGNAL_U8(name, value) constexpr int name = value;
#define RADIOIFY_ASCII_SCALE_256(name, value) constexpr int name = value;
#include "asciiart_constants.inc"
#undef RADIOIFY_ASCII_BOOL
#undef RADIOIFY_ASCII_FLOAT
#undef RADIOIFY_ASCII_COUNT
#undef RADIOIFY_ASCII_LUMA_U8
#undef RADIOIFY_ASCII_SIGNAL_U8
#undef RADIOIFY_ASCII_SCALE_256

// Direct YUV conversie zonder sRGB linearisering
// Rec. 709 coefficients als integer fixed-point (<<16 voor precision)
constexpr int kYCoeffR = (int)(0.2126f * 65536.0f + 0.5f);  // ~13933
constexpr int kYCoeffG = (int)(0.7152f * 65536.0f + 0.5f);  // ~46871
constexpr int kYCoeffB = (int)(0.0722f * 65536.0f + 0.5f);  // ~4732

// Snelle YUV luminantie berekening
FORCE_INLINE uint8_t rgbToY(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t y = (static_cast<uint32_t>(r) * kYCoeffR +
                static_cast<uint32_t>(g) * kYCoeffG +
                static_cast<uint32_t>(b) * kYCoeffB) >>
               16;
  return static_cast<uint8_t>(y > 255 ? 255 : y);
}

void setError(std::string* error, const char* message) {
  if (error) *error = message;
}

FORCE_INLINE float clampFloat(float v, float lo, float hi) {
  return (v < lo) ? lo : (v > hi ? hi : v);
}

FORCE_INLINE int scaleDelta256(int delta, int scale) {
  int bias = (delta >= 0) ? 128 : -128;
  return (delta * scale + bias) / 256;
}

constexpr int kParallelBatchRows = 8;
constexpr int64_t kSmallImageParallelPixelBudget = 96 * 96;
constexpr int64_t kSmallImageParallelCellBudget = 48 * 48;

int hardwareThreadCount() {
  static int count = []() {
    unsigned int hw = std::thread::hardware_concurrency();
    return hw == 0 ? 1 : static_cast<int>(hw);
  }();
  return count;
}

int computeWorkerCount(int totalRows, int minBatch) {
  if (totalRows <= 0) return 1;
  if (totalRows < minBatch * 2) return 1;
  int hw = hardwareThreadCount();
  if (hw <= 1) return 1;
  int maxWorkers = std::min(hw, totalRows / minBatch);
  return std::max(1, maxWorkers);
}

template <typename Fn>
void parallelFor(int totalRows, int minBatch, int workerCount, Fn&& fn) {
  if (workerCount <= 1 || totalRows <= 0) {
    if (totalRows > 0) fn(0, totalRows, 0);
    return;
  }
  std::atomic<int> next{0};
  std::atomic<bool> failed{false};
  std::exception_ptr failure;
  std::mutex failureMutex;
  auto captureFailure = [&](std::exception_ptr ep) {
    if (!ep) return;
    std::lock_guard<std::mutex> lock(failureMutex);
    if (!failure) {
      failure = std::move(ep);
    }
  };
  auto worker = [&](int workerId) {
    try {
      for (;;) {
        if (failed.load(std::memory_order_acquire)) {
          break;
        }
        int start = next.fetch_add(minBatch);
        if (start >= totalRows) break;
        int end = std::min(totalRows, start + minBatch);
        fn(start, end, workerId);
      }
    } catch (...) {
      failed.store(true, std::memory_order_release);
      captureFailure(std::current_exception());
    }
  };
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(workerCount - 1));
  for (int i = 1; i < workerCount; ++i) {
    try {
      threads.emplace_back(worker, i);
    } catch (...) {
      failed.store(true, std::memory_order_release);
      captureFailure(std::current_exception());
      break;
    }
  }
  worker(0);
  for (auto& t : threads) {
    t.join();
  }
  if (failure) {
    std::rethrow_exception(failure);
  }
}

template <bool DebugMode>
FORCE_INLINE bool renderStageEnabled(uint32_t stageMask, uint32_t stage) {
  if constexpr (!DebugMode) {
    (void)stageMask;
    (void)stage;
    return true;
  } else {
    return (stageMask & stage) != 0;
  }
}

struct AsciiTuning {
  int ditherMaxEdge = kDitherMaxEdge;
  int edgeThresholdFloor = kEdgeThresholdFloor;
  int signalStrengthFloor = kSignalStrengthFloor;
  int inkMinLuma = kInkMinLuma;
  int inkMaxScale = kInkMaxScale;
  int colorSaturation = kColorSaturation;
  int inkCoverageMinSignal = kInkCoverageMinSignal;
  int inkVisibleDotCoverage = kInkVisibleDotCoverage;
  int inkCoverageMaxScale = kInkCoverageMaxScale;
  int inkCoverageMinLuma = kInkCoverageMinLuma;
  int edgeMaskFitMinRange = kEdgeMaskFitMinRange;
  int edgeMaskFitMinSignal = kEdgeMaskFitMinSignal;
  int edgeMaskFitMinGain = kEdgeMaskFitMinGain;
  int perceptualLumaErrorWeight = kPerceptualLumaErrorWeight;
  int perceptualBrightBgMinInkContrast =
      kPerceptualBrightBgMinInkContrast;
  int perceptualBrightBgPenalty = kPerceptualBrightBgPenalty;
};

FORCE_INLINE void applyTuningOverride(int& dst, int value) {
  if (value >= 0) dst = value;
}

AsciiTuning resolveAsciiTuning(
    const ascii_debug::RenderOptions* debugOptions) {
  AsciiTuning tuning;
  if (!debugOptions) return tuning;
  const auto& o = debugOptions->tuning;
  applyTuningOverride(tuning.ditherMaxEdge, o.ditherMaxEdge);
  applyTuningOverride(tuning.edgeThresholdFloor, o.edgeThresholdFloor);
  applyTuningOverride(tuning.signalStrengthFloor, o.signalStrengthFloor);
  applyTuningOverride(tuning.inkMinLuma, o.inkMinLuma);
  applyTuningOverride(tuning.inkMaxScale, o.inkMaxScale);
  applyTuningOverride(tuning.colorSaturation, o.colorSaturation);
  applyTuningOverride(tuning.inkCoverageMinSignal,
                      o.inkCoverageMinSignal);
  applyTuningOverride(tuning.inkVisibleDotCoverage,
                      o.inkVisibleDotCoverage);
  applyTuningOverride(tuning.inkCoverageMaxScale, o.inkCoverageMaxScale);
  applyTuningOverride(tuning.inkCoverageMinLuma, o.inkCoverageMinLuma);
  applyTuningOverride(tuning.edgeMaskFitMinRange, o.edgeMaskFitMinRange);
  applyTuningOverride(tuning.edgeMaskFitMinSignal, o.edgeMaskFitMinSignal);
  applyTuningOverride(tuning.edgeMaskFitMinGain, o.edgeMaskFitMinGain);
  applyTuningOverride(tuning.perceptualLumaErrorWeight,
                      o.perceptualLumaErrorWeight);
  applyTuningOverride(tuning.perceptualBrightBgMinInkContrast,
                      o.perceptualBrightBgMinInkContrast);
  applyTuningOverride(tuning.perceptualBrightBgPenalty,
                      o.perceptualBrightBgPenalty);
  return tuning;
}

constexpr int kShadowChromaBoostStart = 12;
constexpr int kShadowChromaBoostFull = 72;
constexpr int kShadowChromaPreserveStrength = 160;
constexpr bool kUseEdgeBoost = true;
constexpr int kEdgeShift = 3;

FORCE_INLINE float expandChromaNorm(int c, bool fullRange, int bitDepth);

FORCE_INLINE int shadowSaturationFromLuma(int y) {
  if (y <= kShadowSatStartLuma) return kShadowMinSaturation;
  if (y >= kShadowSatFullLuma) return 256;
  int numer =
      (y - kShadowSatStartLuma) * (256 - kShadowMinSaturation);
  int denom = kShadowSatFullLuma - kShadowSatStartLuma;
  return kShadowMinSaturation + (numer + denom / 2) / denom;
}

FORCE_INLINE int shadowSaturationWithChroma(int y, int chroma) {
  int keep = shadowSaturationFromLuma(y);
  if (keep >= 256) return 256;
  if (chroma <= kShadowChromaBoostStart) return keep;
  int chromaBoost = 256;
  if (chroma < kShadowChromaBoostFull) {
    int numer = (chroma - kShadowChromaBoostStart) * 256;
    int denom = kShadowChromaBoostFull - kShadowChromaBoostStart;
    chromaBoost = (numer + denom / 2) / denom;
  }
  int shadowWeight = 256 - keep;
  int preserveGain =
      (shadowWeight * chromaBoost * kShadowChromaPreserveStrength +
       (256 * 256) / 2) /
      (256 * 256);
  return std::min(256, keep + ((256 - keep) * preserveGain + 128) / 256);
}

FORCE_INLINE int signalRangeTo255(float signal, int start255, int full255) {
  int signal255 = static_cast<int>(std::lround(
      clampFloat(signal, 0.0f, 1.0f) * 255.0f));
  if (signal255 <= start255) return 0;
  if (signal255 >= full255) return 255;
  int numer = (signal255 - start255) * 255;
  int denom = std::max(1, full255 - start255);
  return (numer + denom / 2) / denom;
}

FORCE_INLINE int sourceBlueConfidenceFromYuv(int u, int v, bool fullRange,
                                             int bitDepth) {
  float uf = expandChromaNorm(u, fullRange, bitDepth);
  float vf = expandChromaNorm(v, fullRange, bitDepth);
  float chromaSignal = std::max(std::abs(uf), std::abs(vf));
  float blueSignal = std::max(uf - std::max(vf, 0.0f) * 0.75f, 0.0f);
  int blue = signalRangeTo255(blueSignal, kSourceBlueSignalStart,
                              kSourceBlueSignalFull);
  int chroma = signalRangeTo255(chromaSignal, kSourceChromaSignalStart,
                                kSourceChromaSignalFull);
  return (blue * chroma + 127) / 255;
}

FORCE_INLINE int boostAdaptiveSatForSourceBlue(int adaptiveSat, int y,
                                               int sourceBlueConfidence) {
  if (sourceBlueConfidence <= 0) return adaptiveSat;
  int darkFactor = std::clamp((60 - y) * 255 / 60, 0, 255);
  int boost = (kSourceBlueInkBoost * sourceBlueConfidence * darkFactor +
               (255 * 255) / 2) /
              (255 * 255);
  return (adaptiveSat * (256 + boost) + 128) >> 8;
}

FORCE_INLINE void applySaturationAroundLuma(uint8_t& r, uint8_t& g,
                                            uint8_t& b, int y,
                                            int saturation256) {
  r = static_cast<uint8_t>(
      std::clamp(y + scaleDelta256(static_cast<int>(r) - y, saturation256), 0,
                 255));
  g = static_cast<uint8_t>(
      std::clamp(y + scaleDelta256(static_cast<int>(g) - y, saturation256), 0,
                 255));
  b = static_cast<uint8_t>(
      std::clamp(y + scaleDelta256(static_cast<int>(b) - y, saturation256), 0,
                 255));
}

FORCE_INLINE void scaleColorToLuma(uint8_t& r, uint8_t& g, uint8_t& b, int y,
                                   int targetY) {
  targetY = std::clamp(targetY, 0, 255);
  if (y <= 0) {
    r = g = b = 0;
    return;
  }
  int scale = (targetY * 256 + y / 2) / y;
  r = static_cast<uint8_t>(
      std::clamp((static_cast<int>(r) * scale + 128) >> 8, 0, 255));
  g = static_cast<uint8_t>(
      std::clamp((static_cast<int>(g) * scale + 128) >> 8, 0, 255));
  b = static_cast<uint8_t>(
      std::clamp((static_cast<int>(b) * scale + 128) >> 8, 0, 255));
}

FORCE_INLINE void compressShadowChroma(uint8_t& r, uint8_t& g, uint8_t& b,
                                       int sourceBlueConfidence = 0) {
  int y =
      (static_cast<int>(r) * 54 + static_cast<int>(g) * 183 +
       static_cast<int>(b) * 19 + 128) >>
      8;
  int maxC = std::max({static_cast<int>(r), static_cast<int>(g),
                       static_cast<int>(b)});
  int minC = std::min({static_cast<int>(r), static_cast<int>(g),
                       static_cast<int>(b)});
  int keep = shadowSaturationWithChroma(y, maxC - minC);
  if (keep < 256) {
    applySaturationAroundLuma(r, g, b, y, keep);
  }

  int blueDominance =
      static_cast<int>(b) - std::max(static_cast<int>(r), static_cast<int>(g));
  int dark =
      std::clamp((kShadowBlueGuardStartLuma - y) * 255 /
                     std::max(1, kShadowBlueGuardRange),
                 0, 255);
  int dominance = 0;
  if (blueDominance >= kShadowBlueDominanceFull) {
    dominance = 255;
  } else if (blueDominance > kShadowBlueDominanceStart) {
    int numer = (blueDominance - kShadowBlueDominanceStart) * 255;
    int denom = kShadowBlueDominanceFull - kShadowBlueDominanceStart;
    dominance = (numer + denom / 2) / denom;
  }
  int relax =
      256 - ((kSourceBlueGuardRelax * sourceBlueConfidence + 127) / 255);
  int guard = ((dark * dominance + 127) / 255 * relax + 128) / 256;
  if (guard <= 0) return;
  int guardKeep =
      256 - (((256 - kShadowBlueGuardKeep) * guard + 127) / 255);
  applySaturationAroundLuma(r, g, b, y, guardKeep);
}

// Rec. 709 coefficients met hogere precisie (fixed-point 16.16)
// Geen per-kanaal lookup tables meer - direct berekening is sneller!

const std::array<uint8_t, 256> kInkLevelFromLum = []() {
  std::array<uint8_t, 256> lut{};
  for (int i = 0; i < 256; ++i) {
    float norm = static_cast<float>(i) / 255.0f;
    float x = kInkUseBright ? norm : (1.0f - norm);
    float coverage = std::pow(x, kInkGamma);
    // Alleen bias toepassen als er daadwerkelijk een verschil is
    // Dit zorgt ervoor dat 0 dots mogelijk is bij zeer lage luminantie
    // verschillen
    if (coverage > 0.001f) {
      coverage = coverage * kCoverageGain + kCoverageBias;
    }
    if (coverage < kCoverageZeroCutoff) coverage = 0.0f;
    coverage = std::clamp(coverage, 0.0f, 1.0f);
    lut[static_cast<size_t>(i)] =
        static_cast<uint8_t>(std::lround(coverage * 255.0f));
  }
  return lut;
}();

// Verbeterde Bayer-achtige dithering matrix geoptimaliseerd voor 2x4 braille
// patroon
const std::array<uint8_t, 8> kDitherThresholdByBit = []() {
  std::array<uint8_t, 8> lut{};
  // Geoptimaliseerde volgorde voor 2x4 braille cel (betere gradiënt verdeling)
  // Positie mapping: [0,1,2,6] = linker kolom, [3,4,5,7] = rechter kolom
  const uint8_t ranks[8] = {0, 4, 2, 6, 1, 5, 3, 7};
  for (int i = 0; i < 8; ++i) {
    // Fijnere thresholds voor betere halftone gradaties
    lut[static_cast<size_t>(i)] =
        static_cast<uint8_t>((ranks[i] * 255 + 4) / 8);
  }
  return lut;
}();

constexpr int kEdgeMaskFitInvalidScore = 0x3fffffff;

FORCE_INLINE int countMaskDots(int mask) {
  int count = 0;
  while (mask != 0) {
    count += mask & 1;
    mask >>= 1;
  }
  return count;
}

FORCE_INLINE int perceptualDisplayError(uint8_t srcR, uint8_t srcG,
                                        uint8_t srcB, int predR, int predG,
                                        int predB, int predY,
                                        const AsciiTuning& tuning) {
  const int dr = static_cast<int>(srcR) - predR;
  const int dg = static_cast<int>(srcG) - predG;
  const int db = static_cast<int>(srcB) - predB;
  const int dy = static_cast<int>(rgbToY(srcR, srcG, srcB)) - predY;
  const int lumaError =
      (dy * dy * tuning.perceptualLumaErrorWeight + 128) >> 8;
  return dr * dr + dg * dg + db * db + lumaError;
}

int edgeMaskFitScore(int mask, int validMask, const uint8_t* rVals,
                     const uint8_t* gVals, const uint8_t* bVals,
                     const uint8_t* bitIds, const uint8_t* validVals,
                     const AsciiTuning& tuning) {
  mask &= validMask;
  int sumOnR = 0;
  int sumOnG = 0;
  int sumOnB = 0;
  int sumOffR = 0;
  int sumOffG = 0;
  int sumOffB = 0;
  int onCount = 0;
  int offCount = 0;

  for (int i = 0; i < 8; ++i) {
    if (!validVals[i]) continue;
    if ((mask & (1 << bitIds[i])) != 0) {
      sumOnR += rVals[i];
      sumOnG += gVals[i];
      sumOnB += bVals[i];
      ++onCount;
    } else {
      sumOffR += rVals[i];
      sumOffG += gVals[i];
      sumOffB += bVals[i];
      ++offCount;
    }
  }

  if (onCount == 0 || offCount == 0) return kEdgeMaskFitInvalidScore;

  const int bgR = (sumOffR + offCount / 2) / offCount;
  const int bgG = (sumOffG + offCount / 2) / offCount;
  const int bgB = (sumOffB + offCount / 2) / offCount;
  const int meanOnR = (sumOnR + onCount / 2) / onCount;
  const int meanOnG = (sumOnG + onCount / 2) / onCount;
  const int meanOnB = (sumOnB + onCount / 2) / onCount;

  const int coverage = std::max(1, tuning.inkVisibleDotCoverage);
  const int scale =
      std::min(tuning.inkCoverageMaxScale,
               (256 * 256 + coverage / 2) / coverage);
  const int fgR =
      std::clamp(bgR + scaleDelta256(meanOnR - bgR, scale), 0, 255);
  const int fgG =
      std::clamp(bgG + scaleDelta256(meanOnG - bgG, scale), 0, 255);
  const int fgB =
      std::clamp(bgB + scaleDelta256(meanOnB - bgB, scale), 0, 255);
  const int predOnR = (fgR * coverage + bgR * (256 - coverage) + 128) >> 8;
  const int predOnG = (fgG * coverage + bgG * (256 - coverage) + 128) >> 8;
  const int predOnB = (fgB * coverage + bgB * (256 - coverage) + 128) >> 8;
  const int bgY = rgbToY(static_cast<uint8_t>(bgR),
                         static_cast<uint8_t>(bgG),
                         static_cast<uint8_t>(bgB));
  const int fgY = rgbToY(static_cast<uint8_t>(fgR),
                         static_cast<uint8_t>(fgG),
                         static_cast<uint8_t>(fgB));
  const int predOnY = rgbToY(static_cast<uint8_t>(predOnR),
                             static_cast<uint8_t>(predOnG),
                             static_cast<uint8_t>(predOnB));

  int score = 0;
  for (int i = 0; i < 8; ++i) {
    if (!validVals[i]) continue;
    const bool on = (mask & (1 << bitIds[i])) != 0;
    const int pr = on ? predOnR : bgR;
    const int pg = on ? predOnG : bgG;
    const int pb = on ? predOnB : bgB;
    const int py = on ? predOnY : bgY;
    score += perceptualDisplayError(rVals[i], gVals[i], bVals[i], pr, pg,
                                    pb, py, tuning);
  }

  if (bgY > fgY && tuning.perceptualBrightBgPenalty > 0) {
    const int visibleInkContrast = ((bgY - fgY) * coverage + 128) >> 8;
    const int missing =
        tuning.perceptualBrightBgMinInkContrast - visibleInkContrast;
    if (missing > 0) {
      int64_t displayScore = score;
      displayScore +=
          (static_cast<int64_t>(missing) * missing * onCount *
               tuning.perceptualBrightBgPenalty +
           128) >>
          8;
      score = static_cast<int>(
          std::min<int64_t>(displayScore, kEdgeMaskFitInvalidScore - 1));
    }
  }

  return score;
}

int fitEdgeMask(int initialMask, int validMask, const uint8_t* rVals,
                const uint8_t* gVals, const uint8_t* bVals,
                const uint8_t* bitIds, const uint8_t* validVals,
                const AsciiTuning& tuning) {
  initialMask &= validMask;
  int bestMask = initialMask;
  int bestScore = edgeMaskFitScore(initialMask, validMask, rVals, gVals, bVals,
                                   bitIds, validVals, tuning);
  const int baseScore = bestScore;

  for (int candidate = validMask; candidate != 0;
       candidate = (candidate - 1) & validMask) {
    if (candidate == validMask) continue;
    int score = edgeMaskFitScore(candidate, validMask, rVals, gVals, bVals,
                                 bitIds, validVals, tuning);
    if (score < bestScore) {
      bestScore = score;
      bestMask = candidate;
    }
  }

  if (bestMask == initialMask || bestScore >= kEdgeMaskFitInvalidScore) {
    return initialMask;
  }
  if (baseScore >= kEdgeMaskFitInvalidScore) {
    return bestMask;
  }
  const int requiredGain = 256 - tuning.edgeMaskFitMinGain;
  if (static_cast<int64_t>(bestScore) * 256 <=
      static_cast<int64_t>(baseScore) * requiredGain) {
    return bestMask;
  }
  return initialMask;
}

FORCE_INLINE uint32_t packRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
         (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

FORCE_INLINE uint8_t clampToByte(float v) {
  if (v < 0.0f) return 0;
  if (v > 255.0f) return 255;
  return static_cast<uint8_t>(v + 0.5f);
}

FORCE_INLINE float expandYNorm(int y, bool fullRange, int bitDepth) {
  int maxCode = (1 << bitDepth) - 1;
  if (y < 0) y = 0;
  if (y > maxCode) y = maxCode;
  int shift = bitDepth > 8 ? (bitDepth - 8) : 0;
  float yMin = fullRange ? 0.0f : static_cast<float>(16 << shift);
  float yMax = fullRange ? static_cast<float>(maxCode)
                         : static_cast<float>(235 << shift);
  float denom = std::max(1.0f, yMax - yMin);
  return clampFloat((static_cast<float>(y) - yMin) / denom, 0.0f, 1.0f);
}

FORCE_INLINE float expandChromaNorm(int c, bool fullRange, int bitDepth) {
  int maxCode = (1 << bitDepth) - 1;
  if (c < 0) c = 0;
  if (c > maxCode) c = maxCode;
  int shift = bitDepth > 8 ? (bitDepth - 8) : 0;
  float cMin = fullRange ? 0.0f : static_cast<float>(16 << shift);
  float cMax = fullRange ? static_cast<float>(maxCode)
                         : static_cast<float>(240 << shift);
  float cMid = static_cast<float>(128 << shift);
  float denom = std::max(1.0f, cMax - cMin);
  return (static_cast<float>(c) - cMid) / denom;
}

[[maybe_unused]] FORCE_INLINE uint8_t to8bitFrom10(int v10) {
  if (v10 < 0) v10 = 0;
  if (v10 > 1023) v10 = 1023;
  return static_cast<uint8_t>((v10 * 255 + 511) / 1023);
}

FORCE_INLINE float pqEotf(float v) {
  constexpr float m1 = 2610.0f / 16384.0f;
  constexpr float m2 = 2523.0f / 32.0f;
  constexpr float c1 = 3424.0f / 4096.0f;
  constexpr float c2 = 2413.0f / 128.0f;
  constexpr float c3 = 2392.0f / 128.0f;
  float vp = std::pow(std::max(v, 0.0f), 1.0f / m2);
  float num = std::max(vp - c1, 0.0f);
  float den = std::max(c2 - c3 * vp, 1e-6f);
  return std::pow(num / den, 1.0f / m1);
}

FORCE_INLINE float hlgEotf(float v) {
  const float a = 0.17883277f;
  const float b = 1.0f - 4.0f * a;
  const float c = 0.5f - a * std::log(4.0f * a);
  v = clampFloat(v, 0.0f, 1.0f);
  if (v <= 0.5f) {
    return (v * v) / 3.0f;
  }
  return (std::exp((v - c) / a) + b) / 12.0f;
}

FORCE_INLINE float toneMapFilmic(float x) {
  constexpr float A = 0.15f;
  constexpr float B = 0.50f;
  constexpr float C = 0.10f;
  constexpr float D = 0.20f;
  constexpr float E = 0.02f;
  constexpr float F = 0.30f;
  return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) -
         E / F;
}

FORCE_INLINE float linearToSrgb(float x) {
  x = std::max(x, 0.0f);
  if (x <= 0.0031308f) return x * 12.92f;
  return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

FORCE_INLINE uint8_t normalizeLuma8(int y, bool fullRange, int bitDepth,
                                    YuvTransfer transfer) {
  float v = expandYNorm(y, fullRange, bitDepth);
  if (transfer == YuvTransfer::Pq) {
    v = pqEotf(v);
    v = toneMapFilmic(v * kHdrScale);
    v = clampFloat(v, 0.0f, 1.0f);
    v = linearToSrgb(v);
  } else if (transfer == YuvTransfer::Hlg) {
    v = hlgEotf(v);
    v = toneMapFilmic(v * kHdrScale);
    v = clampFloat(v, 0.0f, 1.0f);
    v = linearToSrgb(v);
  }
  return clampToByte(clampFloat(v, 0.0f, 1.0f) * 255.0f);
}

FORCE_INLINE void yuvToRgb(int y, int u, int v, bool fullRange, int bitDepth,
                           YuvMatrix matrix, YuvTransfer transfer,
                           uint8_t& outR, uint8_t& outG, uint8_t& outB) {
  float yf = expandYNorm(y, fullRange, bitDepth);
  float uf = expandChromaNorm(u, fullRange, bitDepth);
  float vf = expandChromaNorm(v, fullRange, bitDepth);

  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  if (matrix == YuvMatrix::Bt2020) {
    r = yf + 1.4746f * vf;
    g = yf - 0.16455f * uf - 0.57135f * vf;
    b = yf + 1.8814f * uf;
  } else if (matrix == YuvMatrix::Bt601) {
    r = yf + 1.4020f * vf;
    g = yf - 0.3441f * uf - 0.7141f * vf;
    b = yf + 1.7720f * uf;
  } else {
    r = yf + 1.5748f * vf;
    g = yf - 0.1873f * uf - 0.4681f * vf;
    b = yf + 1.8556f * uf;
  }

  if (transfer == YuvTransfer::Pq || transfer == YuvTransfer::Hlg) {
    r = clampFloat(r, 0.0f, 1.0f);
    g = clampFloat(g, 0.0f, 1.0f);
    b = clampFloat(b, 0.0f, 1.0f);
    if (transfer == YuvTransfer::Pq) {
      r = pqEotf(r);
      g = pqEotf(g);
      b = pqEotf(b);
    } else {
      r = hlgEotf(r);
      g = hlgEotf(g);
      b = hlgEotf(b);
    }
    r = toneMapFilmic(r * kHdrScale);
    g = toneMapFilmic(g * kHdrScale);
    b = toneMapFilmic(b * kHdrScale);
    r = clampFloat(r, 0.0f, 1.0f);
    g = clampFloat(g, 0.0f, 1.0f);
    b = clampFloat(b, 0.0f, 1.0f);
    r = linearToSrgb(r);
    g = linearToSrgb(g);
    b = linearToSrgb(b);
  }

  outR = clampToByte(clampFloat(r, 0.0f, 1.0f) * 255.0f);
  outG = clampToByte(clampFloat(g, 0.0f, 1.0f) * 255.0f);
  outB = clampToByte(clampFloat(b, 0.0f, 1.0f) * 255.0f);
}

// Snelle integer square root approximatie voor edge detection
FORCE_INLINE int fastIntSqrt(int x) {
  if (x <= 0) return 0;
  int r = x;
  int q = 1;
  while (q <= r) q <<= 2;
  while (q > 1) {
    q >>= 2;
    int t = r - q;
    r >>= 1;
    if (t >= 0) {
      r += q;
    }
  }
  return r;
}

struct BrailleFastScratch {
  int srcW = 0;
  int srcH = 0;
  int maxArtWidth = 0;
  int maxHeight = 0;
  int outW = 0;
  int outH = 0;
  int scaledW = 0;
  int scaledH = 0;
  int padStride = 0;

  // Subpixel sample bounds voor elke scaled pixel
  std::vector<int> xMapStart;
  std::vector<int> xMapEnd;
  std::vector<int> yMapStart;
  std::vector<int> yMapEnd;

  std::vector<uint32_t> scaledRGBA;
  std::vector<uint8_t> lumaPad;  // Direct uint8_t - efficiënter
  std::vector<uint8_t> edgeMap;
  std::vector<uint8_t> sourceBlueConfidence;
  bool hasSourceBlueConfidence = false;

  std::vector<uint32_t> prevFg;
  std::vector<uint8_t> prevFgValid;
  std::vector<uint32_t> prevBg;
  std::vector<uint8_t> prevBgValid;
  std::vector<uint8_t> prevMask;
  int prevLumLow = -1;
  int prevLumHigh = -1;
  int prevBgLum = -1;

  void ensure(int w, int h, int maxArtWidthIn, int maxHeightIn) {
    if (w <= 0 || h <= 0) return;
    int maxOutW = std::max(1, maxArtWidthIn - 8);
    int maxOutH =
        (maxHeightIn > 0) ? maxHeightIn : std::max(1, h / 4);
    AsciiArtLayout fitted = fitAsciiArtLayout(w, h, maxOutW, maxOutH);
    int newOutW = fitted.width;
    int newOutH = fitted.height;
    int newScaledW = newOutW * 2;
    int newScaledH = newOutH * 4;
    int newPadStride = newScaledW + 2;

    bool same =
        (w == srcW && h == srcH && maxArtWidthIn == maxArtWidth &&
         maxHeightIn == maxHeight && newOutW == outW && newOutH == outH);
    if (same) return;

    srcW = w;
    srcH = h;
    maxArtWidth = maxArtWidthIn;
    maxHeight = maxHeightIn;
    outW = newOutW;
    outH = newOutH;
    scaledW = newScaledW;
    scaledH = newScaledH;
    padStride = newPadStride;

    // Subpixel sampling bounds - elk scaled pixel samplet meerdere source
    // pixels
    xMapStart.resize(static_cast<size_t>(scaledW));
    xMapEnd.resize(static_cast<size_t>(scaledW));
    yMapStart.resize(static_cast<size_t>(scaledH));
    yMapEnd.resize(static_cast<size_t>(scaledH));

    for (int x = 0; x < scaledW; ++x) {
      xMapStart[x] = (x * w) / scaledW;
      xMapEnd[x] = ((x + 1) * w) / scaledW;
      if (xMapEnd[x] <= xMapStart[x]) xMapEnd[x] = xMapStart[x] + 1;
      if (xMapEnd[x] > w) xMapEnd[x] = w;
    }
    for (int y = 0; y < scaledH; ++y) {
      yMapStart[y] = (y * h) / scaledH;
      yMapEnd[y] = ((y + 1) * h) / scaledH;
      if (yMapEnd[y] <= yMapStart[y]) yMapEnd[y] = yMapStart[y] + 1;
      if (yMapEnd[y] > h) yMapEnd[y] = h;
    }

    scaledRGBA.resize(static_cast<size_t>(scaledW) * scaledH);
    lumaPad.resize(static_cast<size_t>(padStride) * (scaledH + 2));
    edgeMap.resize(static_cast<size_t>(scaledW) * scaledH);
    sourceBlueConfidence.assign(static_cast<size_t>(scaledW) * scaledH, 0);
    prevFg.assign(static_cast<size_t>(outW) * outH, 0);
    prevFgValid.assign(static_cast<size_t>(outW) * outH, 0);
    prevBg.assign(static_cast<size_t>(outW) * outH, 0);
    prevBgValid.assign(static_cast<size_t>(outW) * outH, 0);
    prevMask.assign(static_cast<size_t>(outW) * outH, 0);
    hasSourceBlueConfidence = false;
    prevLumLow = -1;
    prevLumHigh = -1;
    prevBgLum = -1;
  }

  void resetHistory() {
    std::fill(prevFg.begin(), prevFg.end(), 0);
    std::fill(prevFgValid.begin(), prevFgValid.end(), 0);
    std::fill(prevBg.begin(), prevBg.end(), 0);
    std::fill(prevBgValid.begin(), prevBgValid.end(), 0);
    std::fill(prevMask.begin(), prevMask.end(), 0);
    prevLumLow = -1;
    prevLumHigh = -1;
    prevBgLum = -1;
  }
};

template <bool AssumeOpaque, bool DebugMode = false>
bool renderAsciiArtFromScratch(AsciiArt& out, BrailleFastScratch& scratch,
                               uint64_t lumCount, const uint32_t* lumHist,
                               bool isSdr,
                               const ascii_debug::RenderOptions* debugOptions =
                                   nullptr,
                               ascii_debug::RenderStats* debugStats = nullptr) {
  const int outW = scratch.outW;
  const int outH = scratch.outH;
  const int scaledW = scratch.scaledW;
  const int scaledH = scratch.scaledH;
  const int padStride = scratch.padStride;
  AsciiTuning tuning;
  if constexpr (DebugMode) {
    tuning = resolveAsciiTuning(debugOptions);
  }
  uint32_t stageMask = ascii_debug::kAllStages;
  if constexpr (DebugMode) {
    if (debugOptions) {
      stageMask = debugOptions->stageMask;
    }
    if (debugStats) {
      *debugStats = ascii_debug::RenderStats{};
    }
  } else {
    (void)debugOptions;
    (void)debugStats;
  }
  (void)isSdr;

  out.width = outW;
  out.height = outH;
  out.cells.resize(static_cast<size_t>(outW) * outH);

  std::memcpy(scratch.lumaPad.data(), scratch.lumaPad.data() + padStride,
              static_cast<size_t>(padStride));
  std::memcpy(
      scratch.lumaPad.data() + static_cast<size_t>(scaledH + 1) * padStride,
      scratch.lumaPad.data() + static_cast<size_t>(scaledH) * padStride,
      static_cast<size_t>(padStride));

  if constexpr (kUseEdgeBoost) {
    if (!renderStageEnabled<DebugMode>(stageMask,
                                       ascii_debug::kStageEdgeDetect)) {
      std::fill(scratch.edgeMap.begin(), scratch.edgeMap.end(), 0);
    } else {
    int workerCount = computeWorkerCount(scaledH, kParallelBatchRows);
    parallelFor(
        scaledH, kParallelBatchRows, workerCount,
        [&](int yStart, int yEnd, int) {
          for (int y = yStart; y < yEnd; ++y) {
            const uint8_t* RESTRICT row =
                scratch.lumaPad.data() +
                static_cast<size_t>(y + 1) * padStride + 1;
            uint8_t* RESTRICT edgeRow =
                scratch.edgeMap.data() + static_cast<size_t>(y) * scaledW;
            for (int x = 0; x < scaledW; ++x) {
              const uint8_t* p = row + x;
              // Sobel kernel (uint8_t luma sufficient)
              int a00 = p[-padStride - 1];
              int a01 = p[-padStride];
              int a02 = p[-padStride + 1];
              int a10 = p[-1];
              int a12 = p[1];
              int a20 = p[padStride - 1];
              int a21 = p[padStride];
              int a22 = p[padStride + 1];

              // Sobel gradi‰nt
              int gx = (a02 + (a12 << 1) + a22) - (a00 + (a10 << 1) + a20);
              int gy = (a20 + (a21 << 1) + a22) - (a00 + (a01 << 1) + a02);
              int magSq = gx * gx + gy * gy;
              int mag = fastIntSqrt(magSq);
              int edge = mag >> (kEdgeShift - 1);
              if (edge > 255) edge = 255;
              edgeRow[x] = static_cast<uint8_t>(edge);
            }
          }
        });
    }
  }

  int lumLow = 0;
  int lumHigh = 255;
  if (lumCount > 0) {
    uint64_t lowTarget = static_cast<uint64_t>(std::max(
        1.0, std::round(static_cast<double>(lumCount) * kLumLowPercent)));
    uint64_t highTarget = static_cast<uint64_t>(std::max(
        1.0, std::round(static_cast<double>(lumCount) * kLumHighPercent)));
    uint64_t accum = 0;
    for (int i = 0; i < 256; ++i) {
      accum += lumHist[i];
      if (accum >= lowTarget) {
        lumLow = i;
        break;
      }
    }
    accum = 0;
    for (int i = 0; i < 256; ++i) {
      accum += lumHist[i];
      if (accum >= highTarget) {
        lumHigh = i;
        break;
      }
    }
    if (lumHigh <= lumLow) {
      lumHigh = std::min(255, lumLow + 1);
    }
  }
  int bgLum = 255;
  if (lumCount > 0) {
    uint32_t maxCount = 0;
    for (int i = 0; i < 256; ++i) {
      if (lumHist[i] > maxCount) {
        maxCount = lumHist[i];
        bgLum = i;
      }
    }
  }

  if (scratch.prevLumLow >= 0 && scratch.prevLumHigh >= 0 &&
      scratch.prevBgLum >= 0) {
    if (std::abs(lumLow - scratch.prevLumLow) < kLumResetDelta) {
      lumLow = scratch.prevLumLow +
               ((lumLow - scratch.prevLumLow) * kLumSmoothAlpha >> 8);
    }
    if (std::abs(lumHigh - scratch.prevLumHigh) < kLumResetDelta) {
      lumHigh = scratch.prevLumHigh +
                ((lumHigh - scratch.prevLumHigh) * kLumSmoothAlpha >> 8);
    }
    if (std::abs(bgLum - scratch.prevBgLum) < kLumResetDelta) {
      bgLum = scratch.prevBgLum +
              ((bgLum - scratch.prevBgLum) * kLumSmoothAlpha >> 8);
    }
  }
  if (lumHigh <= lumLow) {
    lumHigh = std::min(255, lumLow + 1);
  }
  scratch.prevLumLow = lumLow;
  scratch.prevLumHigh = lumHigh;
  scratch.prevBgLum = bgLum;

  const int lumRange = std::max(80, lumHigh - lumLow);
  const bool useSourceBlueConfidence = scratch.hasSourceBlueConfidence;

  const int brailleMap[2][4] = {{0, 1, 2, 6}, {3, 4, 5, 7}};

  int workerCount = computeWorkerCount(outH, kParallelBatchRows);
  if constexpr (DebugMode) {
    if (debugStats) {
      workerCount = 1;
    }
  }
  parallelFor(
      outH, kParallelBatchRows, workerCount, [&](int cyStart, int cyEnd, int) {
        for (int cy = cyStart; cy < cyEnd; ++cy) {
          int baseY = cy * 4;
          // Prefetch volgende rij data voor betere cache hits
          if (cy + 1 < outH) {
            int nextBaseY = (cy + 1) * 4;
            _mm_prefetch(reinterpret_cast<const char*>(
                             scratch.scaledRGBA.data() +
                             static_cast<size_t>(nextBaseY) * scaledW),
                         _MM_HINT_T0);
          }
          for (int cx = 0; cx < outW; ++cx) {
            int baseX = cx * 2;
            int bitmask = 0;
            int sumAllR = 0;
            int sumAllG = 0;
            int sumAllB = 0;
            int colorCount = 0;
            uint8_t rVals[8];
            uint8_t gVals[8];
            uint8_t bVals[8];
            uint8_t lumVals[8];  // Direct uint8_t
            uint8_t edgeVals[8];
            uint8_t sourceBlueVals[8];
            uint8_t bitIds[8];
            uint8_t validVals[8];
            int validMask = 0;
            int dotIndex = 0;

            // Verzamel lokale statistieken voor adaptive thresholding
            int cellLumMin = 255, cellLumMax = 0;
            int cellLumSum = 0;
            int validCount = 0;
            int cellEdgeMax = 0;

            for (int dy = 0; dy < 4; ++dy) {
              int y = baseY + dy;
              const uint32_t* rgbRow = scratch.scaledRGBA.data() +
                                       static_cast<size_t>(y) * scaledW + baseX;
              const uint8_t* lumRow = scratch.lumaPad.data() +
                                      static_cast<size_t>(y + 1) * padStride +
                                      (baseX + 1);
              const uint8_t* edgeRow = scratch.edgeMap.data() +
                                       static_cast<size_t>(y) * scaledW + baseX;
              const uint8_t* sourceBlueRow =
                  useSourceBlueConfidence
                      ? (scratch.sourceBlueConfidence.data() +
                         static_cast<size_t>(y) * scaledW + baseX)
                      : nullptr;

              for (int dx = 0; dx < 2; ++dx) {
                uint32_t px = rgbRow[dx];
                uint8_t r = static_cast<uint8_t>(px & 0xFF);
                uint8_t g = static_cast<uint8_t>((px >> 8) & 0xFF);
                uint8_t b = static_cast<uint8_t>((px >> 16) & 0xFF);
                uint8_t lum = lumRow[dx];
                uint8_t a = 255;
                if constexpr (!AssumeOpaque) {
                  a = static_cast<uint8_t>((px >> 24) & 0xFF);
                }
                rVals[dotIndex] = r;
                gVals[dotIndex] = g;
                bVals[dotIndex] = b;
                lumVals[dotIndex] = lum;  // Direct uint8_t luma
                edgeVals[dotIndex] = edgeRow[dx];
                sourceBlueVals[dotIndex] =
                    sourceBlueRow ? sourceBlueRow[dx] : 0;
                bitIds[dotIndex] = static_cast<uint8_t>(brailleMap[dx][dy]);
                validVals[dotIndex] = static_cast<uint8_t>(a != 0);

                if constexpr (!AssumeOpaque) {
                  if (a == 0) {
                    ++dotIndex;
                    continue;
                  }
                }
                validMask |= (1 << bitIds[dotIndex]);

                // Lokale statistieken voor deze cel
                if (edgeVals[dotIndex] > cellEdgeMax)
                  cellEdgeMax = edgeVals[dotIndex];
                int lumInt = lum;
                if (lumInt < cellLumMin) cellLumMin = lumInt;
                if (lumInt > cellLumMax) cellLumMax = lumInt;
                cellLumSum += lumInt;
                ++validCount;

                sumAllR += r;
                sumAllG += g;
                sumAllB += b;
                ++colorCount;
                ++dotIndex;
              }
            }

            // Adaptive thresholding: gebruik lokaal contrast indien significant
            int cellLumRange = cellLumMax - cellLumMin;
            int cellLumMean = validCount > 0 ? cellLumSum / validCount : bgLum;
            int cellBgLum = bgLum;
            if (validCount >= 3) {
              cellBgLum = (cellLumSum - cellLumMin - cellLumMax) /
                          (validCount - 2);
            } else if (validCount > 0) {
              cellBgLum = cellLumSum / validCount;
            }
            int alpha = 0;
            if (cellLumRange < 40) {
              alpha = 255;
            } else if (cellLumRange > 90) {
              alpha = 0;
            } else {
              alpha = (90 - cellLumRange) * 255 / (90 - 40);
            }
            int refLum =
                (bgLum * (255 - alpha) + cellBgLum * alpha + 127) / 255;
            bool useLocalThreshold =
                cellLumRange > 20 || cellEdgeMax > tuning.ditherMaxEdge;
            bool useDither = false;

            size_t cellIndex = static_cast<size_t>(cy) * outW + cx;
            uint8_t prevMask = 0;
            if (cellIndex < scratch.prevMask.size()) {
              prevMask = scratch.prevMask[cellIndex];
            }
            int hysteresis =
                (4 * (255 - alpha) + 10 * alpha + 127) / 255;

            // 2. Adaptive Thresholding (Per-Sub-Pixel Logic)
            // This section determines which Braille dots should be active based on the input image.
            // We use a "Per-Sub-Pixel" approach inspired by the reference implementation (asciiart.ts).
            // Instead of trying to match a predefined pattern ("Target Dots"), we evaluate each of the 8 sub-pixels
            // independently to see if it differs significantly from the background. This preserves fine details
            // like single-pixel stars or thin lines that might otherwise be lost in noise reduction.

            // Base threshold (DELTA in ts)
            // This is the minimum luma difference required for a pixel to be considered "foreground".
            // A value of 30 (out of 255) provides a good balance between sensitivity and noise rejection.
            int baseThreshold = 26;
            
            for (int i = 0; i < 8; ++i) {
              if (!validVals[i]) continue;

              int lum = lumVals[i];
              int edge = edgeVals[i];

              // Edge-aware threshold modulation:
              // In areas with strong edges (high detail), we lower the threshold to capture more subtle features.
              // In flat areas (low edge), we keep the threshold high to suppress noise.
              // Formula: threshold = max(10, base - 0.3 * edge)
              // If edge is strong (e.g., 100), threshold drops to 10, making it very sensitive.
              int threshold =
                  std::max(tuning.edgeThresholdFloor,
                           baseThreshold - (edge * 77 / 255));

              // Check against hybrid background luminance:
              // Mix global and local background based on local detail.
              int lumDiff = std::abs(lum - refLum);
              
              // Decision: Is this sub-pixel a dot?
              // If the luma difference exceeds the threshold, it's considered
              // a foreground dot.
              bool wasOn = ((prevMask >> bitIds[i]) & 1) != 0;
              int offThreshold = std::max(6, threshold - hysteresis);
              bool isDot = wasOn ? (lumDiff >= offThreshold)
                                 : (lumDiff >= threshold);

              if (isDot) {
                bitmask |= (1 << bitIds[i]);
              }
            }

            // Calculate stats for contrast logic
            int rawDiff = std::abs(cellLumMean - bgLum);
            int avgLumDiff = validCount > 0 ? rawDiff * 255 / std::max(1, lumRange)
                                            : 0;
            if (avgLumDiff > 255) avgLumDiff = 255;
            int edgeSig = std::clamp((cellEdgeMax - 4) * 255 / 12, 0, 255);
            int lumSig = std::clamp((avgLumDiff - 4) * 255 / 24, 0, 255);
            int signalStrength = std::max(edgeSig, lumSig);

            if (!useLocalThreshold &&
                renderStageEnabled<DebugMode>(stageMask,
                                              ascii_debug::kStageDither)) {
              useDither = true;
              uint8_t coverage =
                  kInkLevelFromLum[static_cast<size_t>(rawDiff)];
              int ditherMask = 0;
              for (int i = 0; i < 8; ++i) {
                if (!validVals[i]) continue;
                if (coverage > kDitherThresholdByBit[bitIds[i]]) {
                  ditherMask |= (1 << bitIds[i]);
                }
              }
              bitmask = ditherMask;
            }

            const bool useEdgeMaskFit =
                renderStageEnabled<DebugMode>(stageMask,
                                              ascii_debug::kStageEdgeMaskFit);
            const bool edgeMaskFitEligible =
                !useDither && useEdgeMaskFit && validCount >= 3 &&
                cellLumRange >= tuning.edgeMaskFitMinRange &&
                signalStrength >= tuning.edgeMaskFitMinSignal;
            if (edgeMaskFitEligible) {
              int fittedMask =
                  fitEdgeMask(bitmask, validMask, rVals, gVals, bVals, bitIds,
                              validVals, tuning);
              if (fittedMask != bitmask) {
                bitmask = fittedMask;
                if constexpr (DebugMode) {
                  if (debugStats) ++debugStats->edgeMaskFitCount;
                }
              }
            }

            int dotCount = countMaskDots(bitmask);
            if constexpr (DebugMode) {
              if (debugStats) {
                ++debugStats->cellCount;
                if (useDither) ++debugStats->ditherCellCount;
                if (cellEdgeMax >= tuning.ditherMaxEdge) {
                  ++debugStats->edgeCellCount;
                }
              }
            }

            uint8_t prevBgR = 0;
            uint8_t prevBgG = 0;
            uint8_t prevBgB = 0;
            bool prevBgValid = (cellIndex < scratch.prevBg.size() &&
                                scratch.prevBgValid[cellIndex]);
            if (prevBgValid) {
              uint32_t p = scratch.prevBg[cellIndex];
              prevBgR = static_cast<uint8_t>((p >> 16) & 0xFF);
              prevBgG = static_cast<uint8_t>((p >> 8) & 0xFF);
              prevBgB = static_cast<uint8_t>(p & 0xFF);
            }
            uint8_t outR = 0;
            uint8_t outG = 0;
            uint8_t outB = 0;
            uint8_t outBgR = 0;
            uint8_t outBgG = 0;
            uint8_t outBgB = 0;
            bool hasBg = false;
            int sumInkR = 0;
            int sumInkG = 0;
            int sumInkB = 0;
            int sumInkSourceBlue = 0;
            int inkCount = 0;
            int sumBgR = 0;
            int sumBgG = 0;
            int sumBgB = 0;
            int sumBgSourceBlue = 0;
            int bgCount = 0;
            int sumAllSourceBlue = 0;
            for (int i = 0; i < 8; ++i) {
              if (!validVals[i]) continue;
              sumAllSourceBlue += sourceBlueVals[i];
              if (bitmask & (1 << bitIds[i])) {
                sumInkR += rVals[i];
                sumInkG += gVals[i];
                sumInkB += bVals[i];
                sumInkSourceBlue += sourceBlueVals[i];
                ++inkCount;
              } else {
                sumBgR += rVals[i];
                sumBgG += gVals[i];
                sumBgB += bVals[i];
                sumBgSourceBlue += sourceBlueVals[i];
                ++bgCount;
              }
            }
            if (colorCount > 0) {
              uint8_t curR =
                  static_cast<uint8_t>((inkCount > 0 ? sumInkR : sumAllR) /
                                       (inkCount > 0 ? inkCount : colorCount));
              uint8_t curG =
                  static_cast<uint8_t>((inkCount > 0 ? sumInkG : sumAllG) /
                                       (inkCount > 0 ? inkCount : colorCount));
              uint8_t curB =
                  static_cast<uint8_t>((inkCount > 0 ? sumInkB : sumAllB) /
                                       (inkCount > 0 ? inkCount : colorCount));
              curR = static_cast<uint8_t>(std::max<int>(curR, kColorLift));
              curG = static_cast<uint8_t>(std::max<int>(curG, kColorLift));
              curB = static_cast<uint8_t>(std::max<int>(curB, kColorLift));
              int curSourceBlue =
                  (inkCount > 0 ? sumInkSourceBlue : sumAllSourceBlue) /
                  (inkCount > 0 ? inkCount : colorCount);

              // Calculate bg color first for blending
              uint8_t bgR =
                  static_cast<uint8_t>((bgCount > 0 ? sumBgR : sumAllR) /
                                       (bgCount > 0 ? bgCount : colorCount));
              uint8_t bgG =
                  static_cast<uint8_t>((bgCount > 0 ? sumBgG : sumAllG) /
                                       (bgCount > 0 ? bgCount : colorCount));
              uint8_t bgB =
                  static_cast<uint8_t>((bgCount > 0 ? sumBgB : sumAllB) /
                                       (bgCount > 0 ? bgCount : colorCount));
              
              // Color Lift for Background as well
              bgR = static_cast<uint8_t>(std::max<int>(bgR, kColorLift));
              bgG = static_cast<uint8_t>(std::max<int>(bgG, kColorLift));
              bgB = static_cast<uint8_t>(std::max<int>(bgB, kColorLift));
              int bgSourceBlue =
                  (bgCount > 0 ? sumBgSourceBlue : sumAllSourceBlue) /
                  (bgCount > 0 ? bgCount : colorCount);

              // Intelligent Contrast Management
              // This system dynamically adjusts contrast based on the "Signal Strength" of the cell.
              // Goal:
              // 1. Noise Suppression: If the signal is weak (noise), blend the foreground into the background
              //    to make it look like a subtle texture rather than hard noise.
              // 2. Detail Enhancement: If the signal is strong (real detail), boost the contrast to make it pop.
              
              // Calculate Signal Strength:
              // We look at both Edge magnitude and Luma difference.
              // - Edge Signal: Ramps from 0 to 1 as edge strength goes from 4 to 16.
              // - Luma Signal: Ramps from 0 to 1 as luma difference goes from 4 to 28.
              int blendStrength =
                  tuning.signalStrengthFloor +
                  ((signalStrength * (255 - tuning.signalStrengthFloor) +
                    127) /
                   255);

              // Dampening (Noise Hiding):
              // If signalStrength is low, we interpolate curFg towards curBg.
              // This effectively "fades out" noise into the background.
              if (renderStageEnabled<DebugMode>(
                      stageMask, ascii_debug::kStageSignalDampen)) {
                if constexpr (DebugMode) {
                  if (debugStats && blendStrength < 255) {
                    ++debugStats->signalDampenCount;
                  }
                }
                curR =
                    static_cast<uint8_t>(bgR +
                                         ((curR - bgR) * blendStrength >> 8));
                curG =
                    static_cast<uint8_t>(bgG +
                                         ((curG - bgG) * blendStrength >> 8));
                curB =
                    static_cast<uint8_t>(bgB +
                                         ((curB - bgB) * blendStrength >> 8));
              }

              // Boosting (Detail Pop):
              // If signalStrength is high (> 0.8), we apply an asymmetrical contrast boost.
              if (renderStageEnabled<DebugMode>(
                      stageMask, ascii_debug::kStageDetailBoost) &&
                  signalStrength > 204) { // > 0.8 * 255
                  if constexpr (DebugMode) {
                    if (debugStats) ++debugStats->detailBoostCount;
                  }
                  int boost = (signalStrength - 204) * 5; // 0 -> 255
                  // Boost factor 1.0 -> 1.5 (approx)
                  // New = Center + (Old - Center) * (1 + boost/512)
                  // Simplified: Expand difference
                  int cR = (curR + bgR) >> 1;
                  int cG = (curG + bgG) >> 1;
                  int cB = (curB + bgB) >> 1;
                  
                  int dR = curR - cR;
                  int dG = curG - cG;
                  int dB = curB - cB;
                  
                  // Asymmetrical Boost Logic:
                  // We want the foreground (dots) to stand out sharply, but we don't want the background
                  // to be crushed to black.
                  
                  // Apply boost to FG (scale delta by 1.5x at max)
                  int scaleFg = 256 + (boost >> 1); // 256 to 384
                  
                  // Apply reduced boost to BG (scale delta by 1.1x at max)
                  // This prevents the "Black Background" issue where high-contrast areas would
                  // push the background color below zero (black), losing context.
                  int scaleBg = 256 + (boost >> 3); // 256 to 288
                  
                  curR = static_cast<uint8_t>(
                      std::clamp(cR + (dR * scaleFg >> 8), 0, 255));
                  curG = static_cast<uint8_t>(
                      std::clamp(cG + (dG * scaleFg >> 8), 0, 255));
                  curB = static_cast<uint8_t>(
                      std::clamp(cB + (dB * scaleFg >> 8), 0, 255));
                  
                  bgR = static_cast<uint8_t>(
                      std::clamp(cR - (dR * scaleBg >> 8), 0, 255));
                  bgG = static_cast<uint8_t>(
                      std::clamp(cG - (dG * scaleBg >> 8), 0, 255));
                  bgB = static_cast<uint8_t>(
                      std::clamp(cB - (dB * scaleBg >> 8), 0, 255));
              }

              int curY =
                  (static_cast<int>(curR) * 54 + static_cast<int>(curG) * 183 +
                   static_cast<int>(curB) * 19 + 128) >>
                  8;
              if (renderStageEnabled<DebugMode>(
                      stageMask, ascii_debug::kStageInkLumaFloor) &&
                  curY < tuning.inkMinLuma) {
                if constexpr (DebugMode) {
                  if (debugStats) ++debugStats->inkLumaFloorCount;
                }
                if (curY <= 0) {
                  curR = static_cast<uint8_t>(tuning.inkMinLuma);
                  curG = static_cast<uint8_t>(tuning.inkMinLuma);
                  curB = static_cast<uint8_t>(tuning.inkMinLuma);
                } else {
                  int scale =
                      (static_cast<int>(tuning.inkMinLuma) * 256) / curY;
                  if (scale > tuning.inkMaxScale) {
                    scale = tuning.inkMaxScale;
                  }
                  curR = static_cast<uint8_t>(std::min(
                      255, (static_cast<int>(curR) * scale + 128) >> 8));
                  curG = static_cast<uint8_t>(std::min(
                      255, (static_cast<int>(curG) * scale + 128) >> 8));
                  curB = static_cast<uint8_t>(std::min(
                      255, (static_cast<int>(curB) * scale + 128) >> 8));
                }
              }
              // Verbeterde kleurverwerking met adaptive saturation
              int y =
                  (static_cast<int>(curR) * 54 + static_cast<int>(curG) * 183 +
                   static_cast<int>(curB) * 19 + 128) >>
                  8;
              // Adaptive Saturation (Ink)
              // Adjust saturation based on brightness.
              // Dark colors get reduced saturation to prevent blue/purple artifacts in shadows.
              int inkMaxC = std::max({static_cast<int>(curR), static_cast<int>(curG),
                                      static_cast<int>(curB)});
              int inkMinC = std::min({static_cast<int>(curR), static_cast<int>(curG),
                                      static_cast<int>(curB)});
              int adaptiveSat =
                  (tuning.colorSaturation *
                       shadowSaturationWithChroma(y, inkMaxC - inkMinC) +
                   128) >>
                  8;
              if (renderStageEnabled<DebugMode>(
                      stageMask, ascii_debug::kStageInkSaturation)) {
                adaptiveSat =
                    boostAdaptiveSatForSourceBlue(adaptiveSat, y, curSourceBlue);
                applySaturationAroundLuma(curR, curG, curB, y, adaptiveSat);
              }

              if (renderStageEnabled<DebugMode>(
                      stageMask,
                      ascii_debug::kStageInkCoverageCompensation) &&
                  !useDither &&
                  signalStrength >= tuning.inkCoverageMinSignal &&
                  bgCount > 0 && inkCount > 0 && dotCount > 0 &&
                  dotCount < validCount) {
                int coverage = std::max(1, tuning.inkVisibleDotCoverage);
                int scale =
                    std::min(tuning.inkCoverageMaxScale,
                             (256 * 256 + coverage / 2) / coverage);
                if (scale > 256) {
                  uint8_t oldR = curR;
                  uint8_t oldG = curG;
                  uint8_t oldB = curB;
                  curR = static_cast<uint8_t>(std::clamp(
                      static_cast<int>(bgR) +
                          scaleDelta256(static_cast<int>(curR) - bgR, scale),
                      0, 255));
                  curG = static_cast<uint8_t>(std::clamp(
                      static_cast<int>(bgG) +
                          scaleDelta256(static_cast<int>(curG) - bgG, scale),
                      0, 255));
                  curB = static_cast<uint8_t>(std::clamp(
                      static_cast<int>(bgB) +
                          scaleDelta256(static_cast<int>(curB) - bgB, scale),
                      0, 255));
                  int coverageY = rgbToY(curR, curG, curB);
                  if (coverageY < tuning.inkCoverageMinLuma) {
                    scaleColorToLuma(curR, curG, curB, coverageY,
                                     tuning.inkCoverageMinLuma);
                  }
                  if constexpr (DebugMode) {
                    if (debugStats &&
                        (oldR != curR || oldG != curG || oldB != curB)) {
                      ++debugStats->inkCoverageCompensationCount;
                    }
                  }
                }
              }

              if (renderStageEnabled<DebugMode>(
                      stageMask, ascii_debug::kStageForegroundTemporal) &&
                  cellIndex < scratch.prevFg.size() &&
                  scratch.prevFgValid[cellIndex]) {
                uint32_t p = scratch.prevFg[cellIndex];
                int pr = static_cast<int>((p >> 16) & 0xFF);
                int pg = static_cast<int>((p >> 8) & 0xFF);
                int pb = static_cast<int>(p & 0xFF);
                int dsum = std::abs(static_cast<int>(curR) - pr) +
                           std::abs(static_cast<int>(curG) - pg) +
                           std::abs(static_cast<int>(curB) - pb);
                if (dsum >= kTemporalResetDelta) {
                  pr = curR;
                  pg = curG;
                  pb = curB;
                } else {
                  if constexpr (DebugMode) {
                    if (debugStats) ++debugStats->fgTemporalBlendCount;
                  }
                  // Temporal Stability (Ghosting Reduction)
                  // We blend the current frame's color with the previous frame's color to reduce flickering.
                  // Increased alpha for faster updates (less ghosting)
                  pr = pr + (((int)curR - pr) * 230 >> 8);
                  pg = pg + (((int)curG - pg) * 230 >> 8);
                  pb = pb + (((int)curB - pb) * 230 >> 8);
                }
                uint8_t fgR = static_cast<uint8_t>(pr);
                uint8_t fgG = static_cast<uint8_t>(pg);
                uint8_t fgB = static_cast<uint8_t>(pb);
                compressShadowChroma(fgR, fgG, fgB, curSourceBlue);
                pr = fgR;
                pg = fgG;
                pb = fgB;
                outR = static_cast<uint8_t>(pr);
                outG = static_cast<uint8_t>(pg);
                outB = static_cast<uint8_t>(pb);
                scratch.prevFg[cellIndex] = (static_cast<uint32_t>(pr) << 16) |
                                            (static_cast<uint32_t>(pg) << 8) |
                                            static_cast<uint32_t>(pb);
              } else {
                outR = curR;
                outG = curG;
                outB = curB;
                compressShadowChroma(outR, outG, outB, curSourceBlue);
                if (cellIndex < scratch.prevFg.size()) {
                  scratch.prevFg[cellIndex] =
                      (static_cast<uint32_t>(outR) << 16) |
                      (static_cast<uint32_t>(outG) << 8) |
                      static_cast<uint32_t>(outB);
                  scratch.prevFgValid[cellIndex] = 1;
                }
              }

              int bgBlendAlpha = 230;

              if (renderStageEnabled<DebugMode>(
                      stageMask, ascii_debug::kStageBackgroundTemporal) &&
                  prevBgValid) {
                int pr = static_cast<int>(prevBgR);
                int pg = static_cast<int>(prevBgG);
                int pb = static_cast<int>(prevBgB);
                int dsum = std::abs(static_cast<int>(bgR) - pr) +
                           std::abs(static_cast<int>(bgG) - pg) +
                           std::abs(static_cast<int>(bgB) - pb);
                if (dsum >= kTemporalResetDelta) {
                  pr = bgR;
                  pg = bgG;
                  pb = bgB;
                } else {
                  if constexpr (DebugMode) {
                    if (debugStats) ++debugStats->bgTemporalBlendCount;
                  }
                  // Temporal Stability (Ghosting Reduction)
                  // Increased alpha for faster updates (less ghosting)
                  pr = pr + (((int)bgR - pr) * bgBlendAlpha >> 8);
                  pg = pg + (((int)bgG - pg) * bgBlendAlpha >> 8);
                  pb = pb + (((int)bgB - pb) * bgBlendAlpha >> 8);
                }
                uint8_t bgOutR = static_cast<uint8_t>(pr);
                uint8_t bgOutG = static_cast<uint8_t>(pg);
                uint8_t bgOutB = static_cast<uint8_t>(pb);
                compressShadowChroma(bgOutR, bgOutG, bgOutB, bgSourceBlue);
                pr = bgOutR;
                pg = bgOutG;
                pb = bgOutB;
                outBgR = static_cast<uint8_t>(pr);
                outBgG = static_cast<uint8_t>(pg);
                outBgB = static_cast<uint8_t>(pb);
                scratch.prevBg[cellIndex] = (static_cast<uint32_t>(pr) << 16) |
                                            (static_cast<uint32_t>(pg) << 8) |
                                            static_cast<uint32_t>(pb);
              } else if (cellIndex < scratch.prevBg.size()) {
                outBgR = bgR;
                outBgG = bgG;
                outBgB = bgB;
                compressShadowChroma(outBgR, outBgG, outBgB, bgSourceBlue);
                scratch.prevBg[cellIndex] = (static_cast<uint32_t>(outBgR) << 16) |
                                            (static_cast<uint32_t>(outBgG) << 8) |
                                            static_cast<uint32_t>(outBgB);
                scratch.prevBgValid[cellIndex] = 1;
              }
              if (renderStageEnabled<DebugMode>(
                      stageMask, ascii_debug::kStageFullMaskBgContrast) &&
                  dotCount == 8 && bgCount == 0) {
                int fgY =
                    (static_cast<int>(outR) * 54 +
                     static_cast<int>(outG) * 183 +
                     static_cast<int>(outB) * 19 + 128) >>
                    8;
                int bgY2 =
                    (static_cast<int>(outBgR) * 54 +
                     static_cast<int>(outBgG) * 183 +
                     static_cast<int>(outBgB) * 19 + 128) >>
                    8;
                int dir = (cellLumMean < bgLum) ? 1 : -1;
                int minDeltaY = 6;
                int need = minDeltaY - dir * (bgY2 - fgY);
                if (need > 0) {
                  if constexpr (DebugMode) {
                    if (debugStats) ++debugStats->fullMaskBgContrastCount;
                  }
                  int shift = dir * need;
                  outBgR = static_cast<uint8_t>(
                      std::clamp(static_cast<int>(outBgR) + shift, 0, 255));
                  outBgG = static_cast<uint8_t>(
                      std::clamp(static_cast<int>(outBgG) + shift, 0, 255));
                  outBgB = static_cast<uint8_t>(
                      std::clamp(static_cast<int>(outBgB) + shift, 0, 255));
                  compressShadowChroma(outBgR, outBgG, outBgB, bgSourceBlue);
                  if (cellIndex < scratch.prevBg.size()) {
                    scratch.prevBg[cellIndex] =
                        (static_cast<uint32_t>(outBgR) << 16) |
                        (static_cast<uint32_t>(outBgG) << 8) |
                        static_cast<uint32_t>(outBgB);
                    scratch.prevBgValid[cellIndex] = 1;
                  }
                }
              }
              hasBg = true;
            } else if (cellIndex < scratch.prevFg.size() &&
                       scratch.prevFgValid[cellIndex]) {
              uint32_t p = scratch.prevFg[cellIndex];
              outR = static_cast<uint8_t>((p >> 16) & 0xFF);
              outG = static_cast<uint8_t>((p >> 8) & 0xFF);
              outB = static_cast<uint8_t>(p & 0xFF);
            }
            if (colorCount == 0) {
              if (cellIndex < scratch.prevFgValid.size()) {
                scratch.prevFgValid[cellIndex] = 0;
              }
              if (cellIndex < scratch.prevBgValid.size()) {
                scratch.prevBgValid[cellIndex] = 0;
              }
              if (cellIndex < scratch.prevMask.size()) {
                scratch.prevMask[cellIndex] = 0;
              }
            }
            if (colorCount > 0 && cellIndex < scratch.prevMask.size()) {
              scratch.prevMask[cellIndex] = static_cast<uint8_t>(bitmask);
            }

            AsciiArt::AsciiCell cell{};

            cell.ch = static_cast<wchar_t>(kBrailleBase + bitmask);
            cell.fg = Color{outR, outG, outB};
            if constexpr (DebugMode) {
              if (debugStats) {
                ++debugStats->dotCountHistogram[std::clamp(dotCount, 0, 8)];
              }
            }
            if (hasBg &&
                renderStageEnabled<DebugMode>(
                    stageMask, ascii_debug::kStageCellBackground)) {
              cell.bg = Color{outBgR, outBgG, outBgB};
              cell.hasBg = true;
              if constexpr (DebugMode) {
                if (debugStats) ++debugStats->bgCellCount;
              }
            }

            out.cells[cellIndex] = cell;
          }
        }
      });

  return true;
}

template <bool AssumeOpaque, bool DebugMode = false>
bool renderAsciiArtFromRgbaFastImpl(const uint8_t* rgba, int width, int height,
                                    int maxWidth, int maxHeight, AsciiArt& out,
                                    BrailleFastScratch& scratch,
                                    const ascii_debug::RenderOptions*
                                        debugOptions = nullptr,
                                    ascii_debug::RenderStats* debugStats =
                                        nullptr) {
  out = AsciiArt{};
  if (!rgba || width <= 0 || height <= 0) return false;

  int maxArtWidth = std::max(8, maxWidth);
  scratch.ensure(width, height, maxArtWidth, maxHeight);
  if (scratch.outW <= 0 || scratch.outH <= 0) return false;
  scratch.hasSourceBlueConfidence = false;

  const int scaledW = scratch.scaledW;
  const int scaledH = scratch.scaledH;
  const int padStride = scratch.padStride;

  uint64_t lumCount = 0;
  uint32_t lumHist[256] = {};
  int workerCount = computeWorkerCount(scaledH, kParallelBatchRows);
  const int64_t scaledPixels =
      static_cast<int64_t>(scaledW) * static_cast<int64_t>(scaledH);
  const int64_t outputCells =
      static_cast<int64_t>(scratch.outW) * static_cast<int64_t>(scratch.outH);
  if (scaledPixels <= kSmallImageParallelPixelBudget ||
      outputCells <= kSmallImageParallelCellBudget) {
    workerCount = 1;
  }
  if constexpr (DebugMode) {
    if (debugStats) {
      workerCount = 1;
    }
  }
  std::vector<std::array<uint32_t, 256>> localHist(workerCount);
  std::vector<uint64_t> localLumCount(workerCount, 0);
  for (auto& hist : localHist) {
    hist.fill(0);
  }

  // Subpixel sampling: middel meerdere source pixels per scaled pixel
  parallelFor(scaledH, kParallelBatchRows, workerCount,
              [&](int yStart, int yEnd, int workerId) {
                auto& hist = localHist[workerId];
                uint64_t lumLocal = 0;
                for (int y = yStart; y < yEnd; ++y) {
                  int syStart = scratch.yMapStart[y];
                  int syEnd = scratch.yMapEnd[y];
                  uint32_t* dstRow = scratch.scaledRGBA.data() +
                                     static_cast<size_t>(y) * scaledW;
                  uint8_t* padRow = scratch.lumaPad.data() +
                                    static_cast<size_t>(y + 1) * padStride;

                  for (int x = 0; x < scaledW; ++x) {
                    int sxStart = scratch.xMapStart[x];
                    int sxEnd = scratch.xMapEnd[x];

                    // Accumuleer alle source pixels in dit bereik
                    uint32_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
                    uint32_t sumLum = 0;
                    int sampleCount = 0;

                    for (int sy = syStart; sy < syEnd; ++sy) {
                      const uint8_t* srcRow =
                          rgba + static_cast<size_t>(sy) * width * 4;
                      for (int sx = sxStart; sx < sxEnd; ++sx) {
                        const uint8_t* sp = srcRow + sx * 4;
                        uint8_t r = sp[0];
                        uint8_t g = sp[1];
                        uint8_t b = sp[2];
                        uint8_t a = 255;
                        if constexpr (!AssumeOpaque) {
                          a = sp[3];
                        }
                        sumR += r;
                        sumG += g;
                        sumB += b;
                        sumA += a;
                        // Directe YUV luminantie berekening (veel sneller
                        // zonder lookup tables)
                        uint8_t lum = rgbToY(r, g, b);
                        sumLum += lum;
                        ++sampleCount;
                      }
                    }

                    // Gemiddelde berekenen
                    if (sampleCount > 0) {
                      uint8_t r = static_cast<uint8_t>(sumR / sampleCount);
                      uint8_t g = static_cast<uint8_t>(sumG / sampleCount);
                      uint8_t b = static_cast<uint8_t>(sumB / sampleCount);
                      uint8_t a = static_cast<uint8_t>(sumA / sampleCount);
                      dstRow[x] = packRGBA(r, g, b, a);

                      uint8_t avgLum = static_cast<uint8_t>(std::min(
                          255U, static_cast<uint32_t>(sumLum / sampleCount)));

                      bool countLum = true;
                      if constexpr (!AssumeOpaque) {
                        if (a == 0) {
                          avgLum = 255;
                          countLum = false;
                        }
                      }
                      padRow[x + 1] = avgLum;
                      if (countLum) {
                        ++hist[avgLum];
                        ++lumLocal;
                      }
                    } else {
                      dstRow[x] = packRGBA(0, 0, 0, 0);
                      padRow[x + 1] = 255;
                    }
                  }
                  padRow[0] = padRow[1];
                  padRow[padStride - 1] = padRow[padStride - 2];
                }
                localLumCount[workerId] += lumLocal;
              });

  for (int i = 0; i < workerCount; ++i) {
    lumCount += localLumCount[i];
    for (int j = 0; j < 256; ++j) {
      lumHist[j] += localHist[i][static_cast<size_t>(j)];
    }
  }

  return renderAsciiArtFromScratch<AssumeOpaque, DebugMode>(
      out, scratch, lumCount, lumHist, true, debugOptions, debugStats);
}

template <bool IsP010>
bool renderAsciiArtFromYuvImpl(const uint8_t* data, int width, int height,
                               int stride, int planeHeight, bool fullRange,
                               YuvMatrix yuvMatrix, YuvTransfer yuvTransfer,
                               int maxWidth, int maxHeight,
                               AsciiArt& out, BrailleFastScratch& scratch) {
  out = AsciiArt{};
  if (!data || width <= 0 || height <= 0 || stride <= 0) return false;

  const int bitDepth = IsP010 ? 10 : 8;

  int maxArtWidth = std::max(8, maxWidth);
  scratch.ensure(width, height, maxArtWidth, maxHeight);
  if (scratch.outW <= 0 || scratch.outH <= 0) return false;
  scratch.hasSourceBlueConfidence = true;

  int effectivePlaneHeight = planeHeight > 0 ? planeHeight : height;
  if (effectivePlaneHeight < height) return false;

  const uint8_t* yPlane = data;
  const uint8_t* uvPlane = data + static_cast<size_t>(stride) *
                                      static_cast<size_t>(effectivePlaneHeight);

  const int scaledW = scratch.scaledW;
  const int scaledH = scratch.scaledH;
  const int padStride = scratch.padStride;

  uint64_t lumCount = 0;
  uint32_t lumHist[256] = {};
  int workerCount = computeWorkerCount(scaledH, kParallelBatchRows);
  std::vector<std::array<uint32_t, 256>> localHist(workerCount);
  std::vector<uint64_t> localLumCount(workerCount, 0);
  for (auto& hist : localHist) {
    hist.fill(0);
  }

  parallelFor(
      scaledH, kParallelBatchRows, workerCount,
      [&](int yStart, int yEnd, int workerId) {
        auto& hist = localHist[workerId];
        uint64_t lumLocal = 0;
        for (int y = yStart; y < yEnd; ++y) {
          int syStart = scratch.yMapStart[y];
          int syEnd = scratch.yMapEnd[y];
          uint32_t* dstRow =
              scratch.scaledRGBA.data() + static_cast<size_t>(y) * scaledW;
          uint8_t* padRow =
              scratch.lumaPad.data() + static_cast<size_t>(y + 1) * padStride;
          uint8_t* sourceBlueRow =
              scratch.sourceBlueConfidence.data() +
              static_cast<size_t>(y) * scaledW;

          for (int x = 0; x < scaledW; ++x) {
            int sxStart = scratch.xMapStart[x];
            int sxEnd = scratch.xMapEnd[x];
            uint64_t sumY = 0;
            uint64_t sumU = 0;
            uint64_t sumV = 0;
            uint32_t sampleCount = 0;

            for (int sy = syStart; sy < syEnd; ++sy) {
              if constexpr (IsP010) {
                const uint16_t* yRow = reinterpret_cast<const uint16_t*>(
                    yPlane + static_cast<size_t>(sy) * stride);
                const uint16_t* uvRow = reinterpret_cast<const uint16_t*>(
                    uvPlane + static_cast<size_t>(sy / 2) * stride);
                for (int sx = sxStart; sx < sxEnd; ++sx) {
                  int uvIndex = (sx / 2) * 2;
                  int y10 = static_cast<int>(yRow[sx] >> 6);
                  int u10 = static_cast<int>(uvRow[uvIndex + 0] >> 6);
                  int v10 = static_cast<int>(uvRow[uvIndex + 1] >> 6);
                  sumY += static_cast<uint64_t>(y10);
                  sumU += static_cast<uint64_t>(u10);
                  sumV += static_cast<uint64_t>(v10);
                  ++sampleCount;
                }
              } else {
                const uint8_t* yRow = yPlane + static_cast<size_t>(sy) * stride;
                const uint8_t* uvRow =
                    uvPlane + static_cast<size_t>(sy / 2) * stride;
                for (int sx = sxStart; sx < sxEnd; ++sx) {
                  int uvIndex = (sx / 2) * 2;
                  int y8 = yRow[sx];
                  int u8 = uvRow[uvIndex + 0];
                  int v8 = uvRow[uvIndex + 1];
                  sumY += static_cast<uint64_t>(y8);
                  sumU += static_cast<uint64_t>(u8);
                  sumV += static_cast<uint64_t>(v8);
                  ++sampleCount;
                }
              }
            }

            if (sampleCount > 0) {
              uint32_t half = sampleCount / 2;
              int yAvg = static_cast<int>((sumY + half) / sampleCount);
              int uAvg = static_cast<int>((sumU + half) / sampleCount);
              int vAvg = static_cast<int>((sumV + half) / sampleCount);

              uint8_t lum =
                  normalizeLuma8(yAvg, fullRange, bitDepth, yuvTransfer);
              padRow[x + 1] = lum;
              ++hist[lum];
              ++lumLocal;

              uint8_t r = 0, g = 0, b = 0;
              yuvToRgb(yAvg, uAvg, vAvg, fullRange, bitDepth, yuvMatrix,
                       yuvTransfer, r, g, b);
              dstRow[x] = packRGBA(r, g, b, 255);
              sourceBlueRow[x] = static_cast<uint8_t>(
                  sourceBlueConfidenceFromYuv(uAvg, vAvg, fullRange, bitDepth));
            } else {
              dstRow[x] = packRGBA(0, 0, 0, 0);
              padRow[x + 1] = 255;
              sourceBlueRow[x] = 0;
            }
          }
          padRow[0] = padRow[1];
          padRow[padStride - 1] = padRow[padStride - 2];
        }
        localLumCount[workerId] += lumLocal;
      });

  for (int i = 0; i < workerCount; ++i) {
    lumCount += localLumCount[i];
    for (int j = 0; j < 256; ++j) {
      lumHist[j] += localHist[i][static_cast<size_t>(j)];
    }
  }

  bool isSdr = (yuvTransfer == YuvTransfer::Sdr);
  return renderAsciiArtFromScratch<true>(out, scratch, lumCount, lumHist,
                                         isSdr);
}

bool renderAsciiArtFromRgbaFast(const uint8_t* rgba, int width, int height,
                                int maxWidth, int maxHeight, AsciiArt& out,
                                bool assumeOpaque,
                                BrailleFastScratch& scratch) {
  if (assumeOpaque) {
    return renderAsciiArtFromRgbaFastImpl<true>(rgba, width, height, maxWidth,
                                                maxHeight, out, scratch);
  }
  return renderAsciiArtFromRgbaFastImpl<false>(rgba, width, height, maxWidth,
                                               maxHeight, out, scratch);
}
}  // namespace

bool renderAsciiArt(const std::filesystem::path& path, int maxWidth,
                    int maxHeight, AsciiArt& out, std::string* error) {
  out = AsciiArt{};
  int imgW = 0;
  int imgH = 0;
  std::vector<uint8_t> rgba;
  if (!decodeImageFileToRgba(path, imgW, imgH, rgba, error)) return false;
  if (imgW <= 0 || imgH <= 0) {
    setError(error, "Image has invalid dimensions.");
    return false;
  }

  static thread_local BrailleFastScratch scratch;
  if (!renderAsciiArtFromRgbaFast(rgba.data(), imgW, imgH, maxWidth, maxHeight,
                                  out, false, scratch)) {
    setError(error, "Failed to render image as ASCII art.");
    return false;
  }
  return true;
}

bool renderAsciiArtFromEncodedImageBytes(const uint8_t* bytes, size_t size,
                                         int maxWidth, int maxHeight,
                                         AsciiArt& out, std::string* error) {
  out = AsciiArt{};
  int imgW = 0;
  int imgH = 0;
  std::vector<uint8_t> rgba;
  if (!decodeImageBytesToRgba(bytes, size, imgW, imgH, rgba, error)) {
    return false;
  }
  if (imgW <= 0 || imgH <= 0) {
    setError(error, "Image has invalid dimensions.");
    return false;
  }

  static thread_local BrailleFastScratch scratch;
  if (!renderAsciiArtFromRgbaFast(rgba.data(), imgW, imgH, maxWidth, maxHeight,
                                  out, false, scratch)) {
    setError(error, "Failed to render image as ASCII art.");
    return false;
  }
  return true;
}

bool renderAsciiArtFromRgba(const uint8_t* rgba, int width, int height,
                            int maxWidth, int maxHeight, AsciiArt& out,
                            bool assumeOpaque) {
  out = AsciiArt{};
  if (!rgba || width <= 0 || height <= 0) return false;
  static thread_local BrailleFastScratch scratch;
  return renderAsciiArtFromRgbaFast(rgba, width, height, maxWidth, maxHeight,
                                    out, assumeOpaque, scratch);
}

bool renderAsciiArtFromRgbaDebug(const uint8_t* rgba, int width, int height,
                                 int maxWidth, int maxHeight, AsciiArt& out,
                                 const ascii_debug::RenderOptions& options,
                                 ascii_debug::RenderStats* stats,
                                 bool assumeOpaque) {
  out = AsciiArt{};
  if (stats) {
    *stats = ascii_debug::RenderStats{};
  }
  if (!rgba || width <= 0 || height <= 0) return false;
  static thread_local BrailleFastScratch scratch;
  if (options.resetHistory) {
    scratch.resetHistory();
  }
  if (assumeOpaque) {
    return renderAsciiArtFromRgbaFastImpl<true, true>(
        rgba, width, height, maxWidth, maxHeight, out, scratch, &options,
        stats);
  }
  return renderAsciiArtFromRgbaFastImpl<false, true>(
      rgba, width, height, maxWidth, maxHeight, out, scratch, &options, stats);
}

bool renderAsciiArtFromYuv(const uint8_t* data, int width, int height,
                           int stride, int planeHeight, YuvFormat format,
                           bool fullRange, YuvMatrix yuvMatrix,
                           YuvTransfer yuvTransfer, int maxWidth,
                           int maxHeight, AsciiArt& out) {
  static thread_local BrailleFastScratch scratch;
  if (format == YuvFormat::P010) {
    return renderAsciiArtFromYuvImpl<true>(data, width, height, stride,
                                           planeHeight, fullRange, yuvMatrix,
                                           yuvTransfer, maxWidth, maxHeight,
                                           out, scratch);
  }
  return renderAsciiArtFromYuvImpl<false>(data, width, height, stride,
                                          planeHeight, fullRange, yuvMatrix,
                                          yuvTransfer, maxWidth, maxHeight,
                                          out, scratch);
}
