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
#include <wincodec.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#ifdef _MSC_VER
#define FORCE_INLINE __forceinline
#define RESTRICT __restrict
#else
#define FORCE_INLINE inline __attribute__((always_inline))
#define RESTRICT __restrict__
#endif

namespace {
constexpr uint32_t kBrailleBase = 0x2800;

// === SIMPLE DENSITY-BASED ASCII RAMP ===
// Classic ASCII art approach: characters sorted by visual density
// This is cached as a simple lookup table indexed by density level (0-15)
constexpr std::array<wchar_t, 16> kDensityRamp = {
    L' ',  // 0 (lightest)
    L'.',  // 1
    L',',  // 2
    L':',  // 3
    L';',  // 4
    L'-',  // 5
    L'=',  // 6
    L'+',  // 7
    L'*',  // 8
    L'#',  // 9
    L'%',  // 10
    L'@',  // 11
    L'░',  // 12
    L'▓',  // 13
    L'▒',  // 14
    L'█'   // 15 (darkest)
};

// Pre-computed: for each dot count (0-8), what density character to use
// This is a simple O(1) lookup, no computation needed at runtime
constexpr std::array<wchar_t, 9> kDotCountToChar = {
    L' ',  // 0 dots
    L'.',  // 1 dot
    L':',  // 2 dots
    L'-',  // 3 dots
    L'+',  // 4 dots
    L'*',  // 5 dots
    L'#',  // 6 dots
    L'%',  // 7 dots
    L'@',  // 8 dots
};

// Configuration: when to use ASCII vs braille
// Braille is better for detail, ASCII is better for large uniform areas
constexpr bool kUseHybridMode = false;  // Disable hybrid - braille is better
constexpr int kMinContrastForBraille =
    15;  // Use braille when cell has this much contrast

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

template <typename T>
void safeRelease(T*& ptr) {
  if (ptr) {
    ptr->Release();
    ptr = nullptr;
  }
}

class ComScope {
 public:
  explicit ComScope(std::string* error) {
    hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr_) && hr_ != RPC_E_CHANGED_MODE) {
      setError(error, "Failed to initialize COM for image decoding.");
    }
  }

  ~ComScope() {
    if (SUCCEEDED(hr_)) CoUninitialize();
  }

  bool ok() const { return SUCCEEDED(hr_) || hr_ == RPC_E_CHANGED_MODE; }

 private:
  HRESULT hr_{E_FAIL};
};

FORCE_INLINE float clampFloat(float v, float lo, float hi) {
  return (v < lo) ? lo : (v > hi ? hi : v);
}

constexpr int kParallelBatchRows = 8;

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
  auto worker = [&](int workerId) {
    for (;;) {
      int start = next.fetch_add(minBatch);
      if (start >= totalRows) break;
      int end = std::min(totalRows, start + minBatch);
      fn(start, end, workerId);
    }
  };
  std::vector<std::thread> threads;
  threads.reserve(static_cast<size_t>(workerCount - 1));
  for (int i = 1; i < workerCount; ++i) {
    threads.emplace_back(worker, i);
  }
  worker(0);
  for (auto& t : threads) {
    t.join();
  }
}

// Verbeterde rendering parameters voor betere precisie
constexpr bool kInkUseBright = true;
constexpr float kInkGamma = 0.65f;      // Slightly steeper
constexpr float kCoverageGain = 1.30f;  // Reduced to prevent early saturation
constexpr float kCoverageBias = 0.0f;   // Removed bias
constexpr float kCoverageZeroCutoff = 0.03f;  // Filter faint noise
constexpr float kLumLowPercent = 0.01f;       // Preciezer dynamic range
constexpr float kLumHighPercent = 0.99f;
constexpr uint8_t kColorLift = 0;
constexpr uint8_t kInkMinLuma = 40;   // Reduced to allow dark details
constexpr uint8_t kBgMinLuma = 10;    // Reduced
constexpr int kInkMaxScale = 1280;  // Verhoogd voor meer bereik
constexpr int kColorAlpha = 48;     // Snellere kleurrespons
constexpr int kBgAlpha = 24;
constexpr int kTemporalResetDelta = 48;  // Snellere scene change detectie
constexpr int kColorSaturation = 340;    // Iets meer saturatie
constexpr bool kUseEdgeBoost = true;
constexpr uint8_t kEdgeMin = 4;      // Reverted to 4 for sensitivity
constexpr uint8_t kEdgeBoost = 245;  // Sterker edge boost
constexpr int kEdgeShift = 3;
constexpr int kBgDelta = 6;  // Lagere threshold voor meer detail
constexpr int kBgDeltaMin = 1;
constexpr int kEdgeDeltaScale = 96;  // Meer edge-responsief
constexpr int kDitherBias = 48;      // Verlaagd voor betere halftones
constexpr bool kUseAABand = true;
constexpr int kAAScoreBandMin = 12;
constexpr int kAAScoreBandMax = 144;  // Soft AA band voor gladdere randen
constexpr int kLumSmoothAlpha = 40;   // Snellere adaptatie
constexpr int kLumResetDelta = 28;

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

const std::array<uint8_t, 256> kEdgeBoostFromMag = []() {
  std::array<uint8_t, 256> lut{};
  if constexpr (kUseEdgeBoost) {
    const int range = 255 - kEdgeMin;
    for (int i = 0; i < 256; ++i) {
      if (i <= kEdgeMin || range <= 0) {
        lut[static_cast<size_t>(i)] = 0;
        continue;
      }
      int boost = (i - kEdgeMin) * kEdgeBoost / range;
      if (boost > kEdgeBoost) boost = kEdgeBoost;
      lut[static_cast<size_t>(i)] = static_cast<uint8_t>(boost);
    }
  }
  return lut;
}();

FORCE_INLINE uint32_t packRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) |
         (static_cast<uint32_t>(b) << 16) | (static_cast<uint32_t>(a) << 24);
}

FORCE_INLINE uint8_t clampToByte(float v) {
  if (v < 0.0f) return 0;
  if (v > 255.0f) return 255;
  return static_cast<uint8_t>(v + 0.5f);
}

FORCE_INLINE uint8_t normalizeLuma8(int y, bool fullRange) {
  if (fullRange) {
    if (y < 0) return 0;
    if (y > 255) return 255;
    return static_cast<uint8_t>(y);
  }
  int v = (y - 16) * 255 / 219;
  if (v < 0) v = 0;
  if (v > 255) v = 255;
  return static_cast<uint8_t>(v);
}

FORCE_INLINE uint8_t to8bitFrom10(int v10) {
  if (v10 < 0) v10 = 0;
  if (v10 > 1023) v10 = 1023;
  return static_cast<uint8_t>((v10 * 255 + 511) / 1023);
}

FORCE_INLINE void yuvToRgb(int y, int u, int v, bool fullRange, uint32_t matrix,
                           uint8_t& outR, uint8_t& outG, uint8_t& outB) {
  float yf = 0.0f;
  float uf = 0.0f;
  float vf = 0.0f;
  if (fullRange) {
    yf = static_cast<float>(y) / 255.0f;
    uf = (static_cast<float>(u) - 128.0f) / 255.0f;
    vf = (static_cast<float>(v) - 128.0f) / 255.0f;
  } else {
    yf = (static_cast<float>(y) - 16.0f) / 219.0f;
    uf = (static_cast<float>(u) - 128.0f) / 224.0f;
    vf = (static_cast<float>(v) - 128.0f) / 224.0f;
  }

  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  if (matrix == MFVideoTransferMatrix_BT709) {
    r = yf + 1.5748f * vf;
    g = yf - 0.1873f * uf - 0.4681f * vf;
    b = yf + 1.8556f * uf;
  } else {
    r = yf + 1.4020f * vf;
    g = yf - 0.3441f * uf - 0.7141f * vf;
    b = yf + 1.7720f * uf;
  }

  outR = clampToByte(r * 255.0f);
  outG = clampToByte(g * 255.0f);
  outB = clampToByte(b * 255.0f);
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
  std::vector<uint8_t> localContrast;  // Per-pixel lokaal contrast

  std::vector<uint32_t> prevFg;
  std::vector<uint8_t> prevFgValid;
  std::vector<uint32_t> prevBg;
  std::vector<uint8_t> prevBgValid;
  int prevLumLow = -1;
  int prevLumHigh = -1;
  int prevBgLum = -1;

  void ensure(int w, int h, int maxArtWidthIn, int maxHeightIn) {
    if (w <= 0 || h <= 0) return;
    int maxOutW = std::max(1, maxArtWidthIn - 8);
    int newOutW = std::max(1, std::min(maxOutW, w / 2));
    int outHAspect = static_cast<int>(
        std::lround(newOutW * (static_cast<float>(h) / w) / 2.0f));
    outHAspect = std::max(1, std::min(outHAspect, h / 4));
    if (maxHeightIn > 0) outHAspect = std::min(outHAspect, maxHeightIn);
    int newOutH = outHAspect;
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
    localContrast.resize(static_cast<size_t>(scaledW) * scaledH);
    prevFg.assign(static_cast<size_t>(outW) * outH, 0);
    prevFgValid.assign(static_cast<size_t>(outW) * outH, 0);
    prevBg.assign(static_cast<size_t>(outW) * outH, 0);
    prevBgValid.assign(static_cast<size_t>(outW) * outH, 0);
    prevLumLow = -1;
    prevLumHigh = -1;
    prevBgLum = -1;
  }
};

bool loadImagePixels(const std::filesystem::path& path, int& outW, int& outH,
                     std::vector<uint8_t>& pixels, std::string* error) {
  ComScope com(error);
  if (!com.ok()) return false;

  IWICImagingFactory* factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    setError(error, "Failed to create image decoder.");
    return false;
  }

  IWICBitmapDecoder* decoder = nullptr;
  std::wstring wpath = path.wstring();
  hr = factory->CreateDecoderFromFilename(wpath.c_str(), nullptr, GENERIC_READ,
                                          WICDecodeMetadataCacheOnDemand,
                                          &decoder);
  if (FAILED(hr)) {
    safeRelease(factory);
    setError(error, "Failed to open image.");
    return false;
  }

  IWICBitmapFrameDecode* frame = nullptr;
  hr = decoder->GetFrame(0, &frame);
  if (FAILED(hr)) {
    safeRelease(decoder);
    safeRelease(factory);
    setError(error, "Failed to decode image frame.");
    return false;
  }

  UINT w = 0;
  UINT h = 0;
  frame->GetSize(&w, &h);
  if (w == 0 || h == 0) {
    safeRelease(frame);
    safeRelease(decoder);
    safeRelease(factory);
    setError(error, "Image has invalid dimensions.");
    return false;
  }
  outW = static_cast<int>(w);
  outH = static_cast<int>(h);

  IWICFormatConverter* converter = nullptr;
  hr = factory->CreateFormatConverter(&converter);
  if (FAILED(hr)) {
    safeRelease(frame);
    safeRelease(decoder);
    safeRelease(factory);
    setError(error, "Failed to convert image.");
    return false;
  }

  hr = converter->Initialize(frame, GUID_WICPixelFormat32bppRGBA,
                             WICBitmapDitherTypeNone, nullptr, 0.0,
                             WICBitmapPaletteTypeCustom);
  if (FAILED(hr)) {
    safeRelease(converter);
    safeRelease(frame);
    safeRelease(decoder);
    safeRelease(factory);
    setError(error, "Failed to convert image format.");
    return false;
  }

  const UINT stride = w * 4;
  const UINT bufferSize = stride * h;
  pixels.assign(bufferSize, 0);
  hr = converter->CopyPixels(nullptr, stride, bufferSize, pixels.data());
  if (FAILED(hr)) {
    safeRelease(converter);
    safeRelease(frame);
    safeRelease(decoder);
    safeRelease(factory);
    setError(error, "Failed to read image pixels.");
    return false;
  }

  safeRelease(converter);
  safeRelease(frame);
  safeRelease(decoder);
  safeRelease(factory);
  return true;
}

template <bool AssumeOpaque>
bool renderAsciiArtFromScratch(AsciiArt& out, BrailleFastScratch& scratch,
                               uint64_t lumCount, const uint32_t* lumHist) {
  const int outW = scratch.outW;
  const int outH = scratch.outH;
  const int scaledW = scratch.scaledW;
  const int scaledH = scratch.scaledH;
  const int padStride = scratch.padStride;

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
            uint8_t* RESTRICT contrastRow =
                scratch.localContrast.data() + static_cast<size_t>(y) * scaledW;
            for (int x = 0; x < scaledW; ++x) {
              const uint8_t* p = row + x;
              // Sobel kernel (uint8_t luma sufficient)
              int a00 = p[-padStride - 1];
              int a01 = p[-padStride];
              int a02 = p[-padStride + 1];
              int a10 = p[-1];
              int a11 = p[0];
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

              // Lokaal contrast: verschil tussen center en gemiddelde
              // neighbours
              int avgNeighbor =
                  (a00 + a01 + a02 + a10 + a12 + a20 + a21 + a22) >> 3;
              int localDiff = std::abs(a11 - avgNeighbor);
              if (localDiff > 255) localDiff = 255;
              contrastRow[x] = static_cast<uint8_t>(localDiff);
            }
          }
        });
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

  const int brailleMap[2][4] = {{0, 1, 2, 6}, {3, 4, 5, 7}};

  int workerCount = computeWorkerCount(outH, kParallelBatchRows);
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
            uint8_t contrastVals[8];
            uint8_t bitIds[8];
            uint8_t validVals[8];
            int dotIndex = 0;

            // Verzamel lokale statistieken voor adaptive thresholding
            int cellLumMin = 255, cellLumMax = 0;
            int cellLumSum = 0;
            int validCount = 0;
            int cellEdgeMax = 0;
            int cellContrastMax = 0;

            for (int dy = 0; dy < 4; ++dy) {
              int y = baseY + dy;
              const uint32_t* rgbRow = scratch.scaledRGBA.data() +
                                       static_cast<size_t>(y) * scaledW + baseX;
              const uint8_t* lumRow = scratch.lumaPad.data() +
                                      static_cast<size_t>(y + 1) * padStride +
                                      (baseX + 1);
              const uint8_t* edgeRow = scratch.edgeMap.data() +
                                       static_cast<size_t>(y) * scaledW + baseX;
              const uint8_t* contrastRow = scratch.localContrast.data() +
                                           static_cast<size_t>(y) * scaledW +
                                           baseX;

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
                contrastVals[dotIndex] = contrastRow[dx];
                bitIds[dotIndex] = static_cast<uint8_t>(brailleMap[dx][dy]);
                validVals[dotIndex] = static_cast<uint8_t>(a != 0);

                if constexpr (!AssumeOpaque) {
                  if (a == 0) {
                    ++dotIndex;
                    continue;
                  }
                }

                // Lokale statistieken voor deze cel
                if (edgeVals[dotIndex] > cellEdgeMax)
                  cellEdgeMax = edgeVals[dotIndex];
                if (contrastVals[dotIndex] > cellContrastMax) {
                  cellContrastMax = contrastVals[dotIndex];
                }
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
                cellLumRange > 20;  // Alleen bij voldoende lokaal contrast
            int localMidpoint =
                useLocalThreshold ? ((cellLumMin + cellLumMax) >> 1) : bgLum;

            // 2. Adaptive Thresholding (Per-Sub-Pixel Logic)
            // This section determines which Braille dots should be active based on the input image.
            // We use a "Per-Sub-Pixel" approach inspired by the reference implementation (asciiart.ts).
            // Instead of trying to match a predefined pattern ("Target Dots"), we evaluate each of the 8 sub-pixels
            // independently to see if it differs significantly from the background. This preserves fine details
            // like single-pixel stars or thin lines that might otherwise be lost in noise reduction.

            // Base threshold (DELTA in ts)
            // This is the minimum luma difference required for a pixel to be considered "foreground".
            // A value of 30 (out of 255) provides a good balance between sensitivity and noise rejection.
            int baseThreshold = 30;
            
            for (int i = 0; i < 8; ++i) {
              if (!validVals[i]) continue;

              int lum = lumVals[i];
              int edge = edgeVals[i];

              // Edge-aware threshold modulation:
              // In areas with strong edges (high detail), we lower the threshold to capture more subtle features.
              // In flat areas (low edge), we keep the threshold high to suppress noise.
              // Formula: threshold = max(10, base - 0.2 * edge)
              // If edge is strong (e.g., 100), threshold drops to 10, making it very sensitive.
              int threshold = std::max(10, baseThreshold - (edge * 51 / 255)); // 0.2 * 255 ~= 51

              // Check against hybrid background luminance:
              // Mix global and local background based on local detail.
              int lumDiff = std::abs(lum - refLum);
              
              // Decision: Is this sub-pixel a dot?
              // If the luma difference exceeds the threshold, it's considered a foreground dot.
              bool isDot = (lumDiff >= threshold);

              if (isDot) {
                bitmask |= (1 << bitIds[i]);
              }
            }

            // Calculate stats for contrast logic
            int rawDiff = std::abs(cellLumMean - bgLum);
            int avgLumDiff = validCount > 0 ? rawDiff * 255 / std::max(1, lumRange)
                                            : 0;
            if (avgLumDiff > 255) avgLumDiff = 255;

            size_t cellIndex = static_cast<size_t>(cy) * outW + cx;
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
            int inkCount = 0;
            int sumBgR = 0;
            int sumBgG = 0;
            int sumBgB = 0;
            int bgCount = 0;
            for (int i = 0; i < 8; ++i) {
              if (!validVals[i]) continue;
              if (bitmask & (1 << bitIds[i])) {
                sumInkR += rVals[i];
                sumInkG += gVals[i];
                sumInkB += bVals[i];
                ++inkCount;
              } else {
                sumBgR += rVals[i];
                sumBgG += gVals[i];
                sumBgB += bVals[i];
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
              int edgeSig = std::clamp((cellEdgeMax - 4) * 255 / 12, 0, 255);
              int lumSig = std::clamp((avgLumDiff - 4) * 255 / 24, 0, 255);
              int signalStrength = std::max(edgeSig, lumSig);

              // Dampening (Noise Hiding):
              // If signalStrength is low, we interpolate curFg towards curBg.
              // This effectively "fades out" noise into the background.
              curR = static_cast<uint8_t>(bgR + ((curR - bgR) * signalStrength >> 8));
              curG = static_cast<uint8_t>(bgG + ((curG - bgG) * signalStrength >> 8));
              curB = static_cast<uint8_t>(bgB + ((curB - bgB) * signalStrength >> 8));

              // Boosting (Detail Pop):
              // If signalStrength is high (> 0.8), we apply an asymmetrical contrast boost.
              if (signalStrength > 204) { // > 0.8 * 255
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
                  
                  curR = static_cast<uint8_t>(std::clamp(cR + (dR * scaleFg >> 8), 0, 255));
                  curG = static_cast<uint8_t>(std::clamp(cG + (dG * scaleFg >> 8), 0, 255));
                  curB = static_cast<uint8_t>(std::clamp(cB + (dB * scaleFg >> 8), 0, 255));
                  
                  bgR = static_cast<uint8_t>(std::clamp(cR - (dR * scaleBg >> 8), 0, 255));
                  bgG = static_cast<uint8_t>(std::clamp(cG - (dG * scaleBg >> 8), 0, 255));
                  bgB = static_cast<uint8_t>(std::clamp(cB - (dB * scaleBg >> 8), 0, 255));
              }

              int curY =
                  (static_cast<int>(curR) * 54 + static_cast<int>(curG) * 183 +
                   static_cast<int>(curB) * 19 + 128) >>
                  8;
              if (curY < kInkMinLuma) {
                if (curY <= 0) {
                  curR = kInkMinLuma;
                  curG = kInkMinLuma;
                  curB = kInkMinLuma;
                } else {
                  int scale = (static_cast<int>(kInkMinLuma) * 256) / curY;
                  if (scale > kInkMaxScale) scale = kInkMaxScale;
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
              int adaptiveSat = kColorSaturation + ((255 - y) >> 2);
              if (adaptiveSat > 400) adaptiveSat = 400;
              curR = static_cast<uint8_t>(std::clamp(
                  y + ((static_cast<int>(curR) - y) * adaptiveSat >> 8), 0,
                  255));
              curG = static_cast<uint8_t>(std::clamp(
                  y + ((static_cast<int>(curG) - y) * adaptiveSat >> 8), 0,
                  255));
              curB = static_cast<uint8_t>(std::clamp(
                  y + ((static_cast<int>(curB) - y) * adaptiveSat >> 8), 0,
                  255));

              if (cellIndex < scratch.prevFg.size() &&
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
                  // Temporal Stability (Ghosting Reduction)
                  // We blend the current frame's color with the previous frame's color to reduce flickering.
                  // Increased alpha for faster updates (less ghosting)
                  // Old: kColorAlpha (180), New: 230
                  pr = pr + (((int)curR - pr) * 230 >> 8);
                  pg = pg + (((int)curG - pg) * 230 >> 8);
                  pb = pb + (((int)curB - pb) * 230 >> 8);
                }
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
                if (cellIndex < scratch.prevFg.size()) {
                  scratch.prevFg[cellIndex] =
                      (static_cast<uint32_t>(curR) << 16) |
                      (static_cast<uint32_t>(curG) << 8) |
                      static_cast<uint32_t>(curB);
                  scratch.prevFgValid[cellIndex] = 1;
                }
              }

              // bgR/G/B already calculated above for blending
              int bgY =
                  (static_cast<int>(bgR) * 54 + static_cast<int>(bgG) * 183 +
                   static_cast<int>(bgB) * 19 + 128) >>
                  8;
              if (bgY < kBgMinLuma) {
                if (bgY <= 0) {
                  bgR = kBgMinLuma;
                  bgG = kBgMinLuma;
                  bgB = kBgMinLuma;
                } else {
                  int scale = (static_cast<int>(kBgMinLuma) * 256) / bgY;
                  bgR = static_cast<uint8_t>(std::min(
                      255, (static_cast<int>(bgR) * scale + 128) >> 8));
                  bgG = static_cast<uint8_t>(std::min(
                      255, (static_cast<int>(bgG) * scale + 128) >> 8));
                  bgB = static_cast<uint8_t>(std::min(
                      255, (static_cast<int>(bgB) * scale + 128) >> 8));
                }
              }
              if (cellIndex < scratch.prevBg.size() &&
                  scratch.prevBgValid[cellIndex]) {
                uint32_t p = scratch.prevBg[cellIndex];
                int pr = static_cast<int>((p >> 16) & 0xFF);
                int pg = static_cast<int>((p >> 8) & 0xFF);
                int pb = static_cast<int>(p & 0xFF);
                int dsum = std::abs(static_cast<int>(bgR) - pr) +
                           std::abs(static_cast<int>(bgG) - pg) +
                           std::abs(static_cast<int>(bgB) - pb);
                if (dsum >= kTemporalResetDelta) {
                  pr = bgR;
                  pg = bgG;
                  pb = bgB;
                } else {
                  // Temporal Stability (Ghosting Reduction)
                  // Increased alpha for faster updates (less ghosting)
                  // Old: kBgAlpha (140), New: 230
                  pr = pr + (((int)bgR - pr) * 230 >> 8);
                  pg = pg + (((int)bgG - pg) * 230 >> 8);
                  pb = pb + (((int)bgB - pb) * 230 >> 8);
                }
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
                scratch.prevBg[cellIndex] = (static_cast<uint32_t>(bgR) << 16) |
                                            (static_cast<uint32_t>(bgG) << 8) |
                                            static_cast<uint32_t>(bgB);
                scratch.prevBgValid[cellIndex] = 1;
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
            }

            AsciiArt::AsciiCell cell{};

            // === CHARACTER SELECTION ===
            // Braille geeft de beste resultaten voor detail - gebruik het
            // altijd De hybrid mode is optioneel voor gebieden zonder contrast
            wchar_t finalChar = static_cast<wchar_t>(kBrailleBase + bitmask);

            if constexpr (kUseHybridMode) {
              // Alleen voor zeer uniforme gebieden: gebruik density-based ASCII
              if (cellLumRange < kMinContrastForBraille) {
                // Tel dots voor density lookup (O(1) met popcount)
                int dotCount = 0;
                int b = bitmask;
                while (b) {
                  dotCount += (b & 1);
                  b >>= 1;
                }
                finalChar = kDotCountToChar[static_cast<size_t>(dotCount)];
              }
            }

            cell.ch = finalChar;
            cell.fg = Color{outR, outG, outB};
            if (hasBg) {
              cell.bg = Color{outBgR, outBgG, outBgB};
              cell.hasBg = true;
            }

            out.cells[cellIndex] = cell;
          }
        }
      });

  return true;
}

template <bool AssumeOpaque>
bool renderAsciiArtFromRgbaFastImpl(const uint8_t* rgba, int width, int height,
                                    int maxWidth, int maxHeight, AsciiArt& out,
                                    BrailleFastScratch& scratch) {
  out = AsciiArt{};
  if (!rgba || width <= 0 || height <= 0) return false;

  int maxArtWidth = std::max(8, maxWidth);
  scratch.ensure(width, height, maxArtWidth, maxHeight);
  if (scratch.outW <= 0 || scratch.outH <= 0) return false;

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

  return renderAsciiArtFromScratch<AssumeOpaque>(out, scratch, lumCount,
                                                 lumHist);
}

template <bool IsP010>
bool renderAsciiArtFromYuvImpl(const uint8_t* data, int width, int height,
                               int stride, int planeHeight, bool fullRange,
                               uint32_t yuvMatrix, int maxWidth, int maxHeight,
                               AsciiArt& out, BrailleFastScratch& scratch) {
  out = AsciiArt{};
  if (!data || width <= 0 || height <= 0 || stride <= 0) return false;

  int maxArtWidth = std::max(8, maxWidth);
  scratch.ensure(width, height, maxArtWidth, maxHeight);
  if (scratch.outW <= 0 || scratch.outH <= 0) return false;

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
              uint8_t y8 = 0;
              uint8_t u8 = 0;
              uint8_t v8 = 0;
              if constexpr (IsP010) {
                y8 = to8bitFrom10(yAvg);
                u8 = to8bitFrom10(uAvg);
                v8 = to8bitFrom10(vAvg);
              } else {
                y8 = static_cast<uint8_t>(std::clamp(yAvg, 0, 255));
                u8 = static_cast<uint8_t>(std::clamp(uAvg, 0, 255));
                v8 = static_cast<uint8_t>(std::clamp(vAvg, 0, 255));
              }

              uint8_t lum = normalizeLuma8(y8, fullRange);
              padRow[x + 1] = lum;
              ++hist[lum];
              ++lumLocal;

              uint8_t r = 0, g = 0, b = 0;
              yuvToRgb(static_cast<int>(y8), static_cast<int>(u8),
                       static_cast<int>(v8), fullRange, yuvMatrix, r, g, b);
              dstRow[x] = packRGBA(r, g, b, 255);
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

  return renderAsciiArtFromScratch<true>(out, scratch, lumCount, lumHist);
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

std::string defaultAsciiRamp() { return "@%#*+=-:. "; }

char rampCharFromLuma(float luma, const std::string& ramp) {
  if (ramp.empty()) return ' ';
  luma = clampFloat(luma, 0.0f, 1.0f);
  float scaled = luma * static_cast<float>(ramp.size() - 1);
  size_t idx = static_cast<size_t>(std::lround(scaled));
  if (idx >= ramp.size()) idx = ramp.size() - 1;
  return ramp[idx];
}

bool renderAsciiArt(const std::filesystem::path& path, int maxWidth,
                    int maxHeight, AsciiArt& out, std::string* error) {
  out = AsciiArt{};
  int imgW = 0;
  int imgH = 0;
  std::vector<uint8_t> rgba;
  if (!loadImagePixels(path, imgW, imgH, rgba, error)) return false;
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

bool renderAsciiArtFromYuv(const uint8_t* data, int width, int height,
                           int stride, int planeHeight, YuvFormat format,
                           bool fullRange, uint32_t yuvMatrix, int maxWidth,
                           int maxHeight, AsciiArt& out) {
  static thread_local BrailleFastScratch scratch;
  if (format == YuvFormat::P010) {
    return renderAsciiArtFromYuvImpl<true>(data, width, height, stride,
                                           planeHeight, fullRange, yuvMatrix,
                                           maxWidth, maxHeight, out, scratch);
  }
  return renderAsciiArtFromYuvImpl<false>(data, width, height, stride,
                                          planeHeight, fullRange, yuvMatrix,
                                          maxWidth, maxHeight, out, scratch);
}
