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
  kStageMinorityBgSwap = 1u << 2,
  kStageBrightBgSwap = 1u << 3,
  kStageEdgeBgTone = 1u << 4,
  kStageSignalDampen = 1u << 5,
  kStageDetailBoost = 1u << 6,
  kStageInkLumaFloor = 1u << 7,
  kStageInkSaturation = 1u << 8,
  kStageForegroundTemporal = 1u << 9,
  kStageBgLumaFloor = 1u << 10,
  kStageBackgroundTemporal = 1u << 11,
  kStageFullMaskBgContrast = 1u << 12,
  kStageEdgeBgClamp = 1u << 13,
  kStageCellBackground = 1u << 14,
};

constexpr uint32_t kAllStages =
    kStageEdgeDetect | kStageDither | kStageMinorityBgSwap |
    kStageBrightBgSwap | kStageEdgeBgTone | kStageSignalDampen |
    kStageDetailBoost | kStageInkLumaFloor | kStageInkSaturation |
    kStageForegroundTemporal | kStageBgLumaFloor |
    kStageBackgroundTemporal | kStageFullMaskBgContrast |
    kStageEdgeBgClamp | kStageCellBackground;

struct RenderOptions {
  uint32_t stageMask = kAllStages;
  bool resetHistory = true;
};

struct RenderStats {
  uint64_t cellCount = 0;
  uint64_t bgCellCount = 0;
  uint64_t ditherCellCount = 0;
  uint64_t edgeCellCount = 0;
  uint64_t minorityBgSwapCount = 0;
  uint64_t brightBgSwapCount = 0;
  uint64_t edgeBgToneCount = 0;
  uint64_t signalDampenCount = 0;
  uint64_t detailBoostCount = 0;
  uint64_t inkLumaFloorCount = 0;
  uint64_t bgLumaFloorCount = 0;
  uint64_t fgTemporalBlendCount = 0;
  uint64_t bgTemporalBlendCount = 0;
  uint64_t fullMaskBgContrastCount = 0;
  uint64_t edgeBgClampCount = 0;
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
