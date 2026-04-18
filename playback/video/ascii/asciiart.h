#ifndef ASCIIART_H
#define ASCIIART_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "consolescreen.h"
#include "videocolor.h"

struct AsciiArt {
  int width = 0;
  int height = 0;
  struct AsciiCell {
    wchar_t ch = L' ';
    Color fg{255, 255, 255};
    Color bg{0, 0, 0};
    bool hasBg = false;
  };
  std::vector<AsciiCell> cells;
};

namespace ascii_debug {

enum StageMask : uint32_t {
  kStageEdgeDetect = 1u << 0,
  kStageDither = 1u << 1,
  kStageBrightBgSwap = 1u << 2,
  kStageSignalDampen = 1u << 3,
  kStageDetailBoost = 1u << 4,
  kStageEdgeMaskFit = 1u << 5,
  kStageInkCoverageCompensation = 1u << 6,
  kStageInkLumaFloor = 1u << 7,
  kStageInkSaturation = 1u << 8,
  kStageForegroundTemporal = 1u << 9,
  kStageBgLumaFloor = 1u << 10,
  kStageBackgroundTemporal = 1u << 11,
  kStageFullMaskBgContrast = 1u << 12,
  kStageCellBackground = 1u << 13,
};

constexpr uint32_t kAllStages =
    kStageEdgeDetect | kStageDither | kStageBrightBgSwap |
    kStageSignalDampen | kStageDetailBoost | kStageEdgeMaskFit |
    kStageInkCoverageCompensation | kStageInkLumaFloor |
    kStageInkSaturation | kStageForegroundTemporal |
    kStageBgLumaFloor | kStageBackgroundTemporal |
    kStageFullMaskBgContrast | kStageCellBackground;

struct RenderOptions {
  uint32_t stageMask = kAllStages;
  bool resetHistory = true;

  struct TuningOverrides {
    int ditherMaxEdge = -1;
    int edgeThresholdFloor = -1;
    int signalStrengthFloor = -1;
    int inkMinLuma = -1;
    int bgMinLuma = -1;
    int inkMaxScale = -1;
    int colorSaturation = -1;
    int inkCoverageMinSignal = -1;
    int inkVisibleDotCoverage = -1;
    int inkCoverageMaxScale = -1;
    int inkCoverageMinLuma = -1;
    int edgeMaskFitMinRange = -1;
    int edgeMaskFitMinSignal = -1;
    int edgeMaskFitMinGain = -1;
    int edgeMaskBgSurfaceWeight = -1;
    int brightBgSwapDelta = -1;
    int brightBgSwapMaxDots = -1;
    int brightBgSwapMinSignal = -1;
    int brightBgSwapScoreMinGain = -1;
  } tuning;
};

struct RenderStats {
  uint64_t cellCount = 0;
  uint64_t bgCellCount = 0;
  uint64_t ditherCellCount = 0;
  uint64_t edgeCellCount = 0;
  uint64_t brightBgSwapCount = 0;
  uint64_t brightBgSwapRejectedByScoreCount = 0;
  uint64_t signalDampenCount = 0;
  uint64_t detailBoostCount = 0;
  uint64_t edgeMaskFitCount = 0;
  uint64_t inkCoverageCompensationCount = 0;
  uint64_t inkLumaFloorCount = 0;
  uint64_t bgLumaFloorCount = 0;
  uint64_t fgTemporalBlendCount = 0;
  uint64_t bgTemporalBlendCount = 0;
  uint64_t fullMaskBgContrastCount = 0;
  uint64_t dotCountHistogram[9] = {};
};

}  // namespace ascii_debug

bool renderAsciiArt(const std::filesystem::path& path,
                    int maxWidth,
                    int maxHeight,
                    AsciiArt& out,
                    std::string* error);

bool renderAsciiArtFromEncodedImageBytes(const uint8_t* bytes,
                                         size_t size,
                                         int maxWidth,
                                         int maxHeight,
                                         AsciiArt& out,
                                         std::string* error);

bool renderAsciiArtFromRgba(const uint8_t* rgba,
                            int width,
                            int height,
                            int maxWidth,
                            int maxHeight,
                            AsciiArt& out,
                            bool assumeOpaque = false);

bool renderAsciiArtFromRgbaDebug(const uint8_t* rgba,
                                 int width,
                                 int height,
                                 int maxWidth,
                                 int maxHeight,
                                 AsciiArt& out,
                                 const ascii_debug::RenderOptions& options,
                                 ascii_debug::RenderStats* stats = nullptr,
                                 bool assumeOpaque = false);

enum class YuvFormat {
  NV12,
  P010,
};

bool renderAsciiArtFromYuv(const uint8_t* data,
                           int width,
                           int height,
                           int stride,
                           int planeHeight,
                           YuvFormat format,
                           bool fullRange,
                           YuvMatrix yuvMatrix,
                           YuvTransfer yuvTransfer,
                           int maxWidth,
                           int maxHeight,
                           AsciiArt& out);

#endif
