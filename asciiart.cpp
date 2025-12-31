#include "asciiart.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <wincodec.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
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
  L' ',   // 0: empty
  L'.',   // 1: very light
  L':',   // 2
  L'-',   // 3
  L'=',   // 4
  L'+',   // 5
  L'*',   // 6
  L'#',   // 7
  L'%',   // 8
  L'@',   // 9
  L'@',   // 10
  L'@',   // 11
  L'@',   // 12
  L'@',   // 13
  L'@',   // 14
  L'@',   // 15: full
};

// Pre-computed: for each dot count (0-8), what density character to use
// This is a simple O(1) lookup, no computation needed at runtime
constexpr std::array<wchar_t, 9> kDotCountToChar = {
  L' ',   // 0 dots
  L'.',   // 1 dot
  L':',   // 2 dots
  L'-',   // 3 dots
  L'+',   // 4 dots
  L'*',   // 5 dots
  L'#',   // 6 dots
  L'%',   // 7 dots
  L'@',   // 8 dots
};

// Configuration: when to use ASCII vs braille
// Braille is better for detail, ASCII is better for large uniform areas
constexpr bool kUseHybridMode = false;          // Disable hybrid - braille is better
constexpr int kMinContrastForBraille = 15;      // Use braille when cell has this much contrast

const std::array<float, 256> kSrgbToLinear = []() {
  std::array<float, 256> table{};
  for (int i = 0; i < 256; ++i) {
    float s = static_cast<float>(i) / 255.0f;
    table[i] =
        (s <= 0.04045f) ? (s / 12.92f) : std::pow((s + 0.055f) / 1.055f, 2.4f);
  }
  return table;
}();

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

// Verbeterde rendering parameters voor betere precisie
constexpr bool kInkUseBright = true;
constexpr float kInkGamma = 0.50f;           // Iets lineairder voor betere detail
constexpr float kCoverageGain = 1.85f;       // Verhoogd voor meer contrast
constexpr float kCoverageBias = 0.12f;       // Verlaagd voor schonere achtergrond
constexpr float kCoverageZeroCutoff = 0.02f; // Iets hoger voor minder ruis
constexpr float kLumLowPercent = 0.01f;      // Preciezer dynamic range
constexpr float kLumHighPercent = 0.99f;
constexpr uint8_t kColorLift = 0;
constexpr uint8_t kInkMinLuma = 110;         // Iets verlaagd voor donkerdere tinten
constexpr uint8_t kBgMinLuma = 20;
constexpr int kInkMaxScale = 1280;           // Verhoogd voor meer bereik
constexpr int kColorAlpha = 48;              // Snellere kleurrespons
constexpr int kBgAlpha = 24;
constexpr int kTemporalResetDelta = 48;      // Snellere scene change detectie
constexpr int kColorSaturation = 340;        // Iets meer saturatie
constexpr bool kUseEdgeBoost = true;
constexpr uint8_t kEdgeMin = 4;              // Lagere drempel voor fijnere edges
constexpr uint8_t kEdgeBoost = 245;          // Sterker edge boost
constexpr int kEdgeShift = 3;
constexpr int kBgDelta = 6;                  // Lagere threshold voor meer detail
constexpr int kBgDeltaMin = 1;
constexpr int kEdgeDeltaScale = 96;          // Meer edge-responsief
constexpr int kDitherBias = 48;              // Verlaagd voor betere halftones
constexpr int kLumSmoothAlpha = 40;          // Snellere adaptatie
constexpr int kLumResetDelta = 28;

// Rec. 709 coefficients met hogere precisie (fixed-point 16.16)
const std::array<uint32_t, 256> kYFromR = []() {
  std::array<uint32_t, 256> t{};
  for (int i = 0; i < 256; ++i) {
    // Gebruik 16-bit fractie voor hogere precisie
    float v = 65535.0f * (0.2126f * kSrgbToLinear[i]);
    t[i] = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 65535));
  }
  return t;
}();

const std::array<uint32_t, 256> kYFromG = []() {
  std::array<uint32_t, 256> t{};
  for (int i = 0; i < 256; ++i) {
    float v = 65535.0f * (0.7152f * kSrgbToLinear[i]);
    t[i] = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 65535));
  }
  return t;
}();

const std::array<uint32_t, 256> kYFromB = []() {
  std::array<uint32_t, 256> t{};
  for (int i = 0; i < 256; ++i) {
    float v = 65535.0f * (0.0722f * kSrgbToLinear[i]);
    t[i] = static_cast<uint32_t>(std::clamp(static_cast<int>(std::lround(v)), 0, 65535));
  }
  return t;
}();

const std::array<uint8_t, 256> kInkLevelFromLum = []() {
  std::array<uint8_t, 256> lut{};
  for (int i = 0; i < 256; ++i) {
    float norm = static_cast<float>(i) / 255.0f;
    float x = kInkUseBright ? norm : (1.0f - norm);
    float coverage = std::pow(x, kInkGamma);
    // Alleen bias toepassen als er daadwerkelijk een verschil is
    // Dit zorgt ervoor dat 0 dots mogelijk is bij zeer lage luminantie verschillen
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

// Verbeterde Bayer-achtige dithering matrix geoptimaliseerd voor 2x4 braille patroon
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
  return static_cast<uint32_t>(r) |
         (static_cast<uint32_t>(g) << 8) |
         (static_cast<uint32_t>(b) << 16) |
         (static_cast<uint32_t>(a) << 24);
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
  std::vector<uint16_t> lumaPad16;  // 16-bit voor hogere precisie
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

    // Subpixel sampling bounds - elk scaled pixel samplet meerdere source pixels
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
    lumaPad16.resize(static_cast<size_t>(padStride) * (scaledH + 2));
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
bool renderAsciiArtFromRgbaFastImpl(const uint8_t* rgba, int width, int height,
                                    int maxWidth, int maxHeight, AsciiArt& out,
                                    BrailleFastScratch& scratch) {
  out = AsciiArt{};
  if (!rgba || width <= 0 || height <= 0) return false;

  int maxArtWidth = std::max(8, maxWidth);
  scratch.ensure(width, height, maxArtWidth, maxHeight);
  if (scratch.outW <= 0 || scratch.outH <= 0) return false;

  const int outW = scratch.outW;
  const int outH = scratch.outH;
  const int scaledW = scratch.scaledW;
  const int scaledH = scratch.scaledH;
  const int padStride = scratch.padStride;

  out.width = outW;
  out.height = outH;
  out.cells.resize(static_cast<size_t>(outW) * outH);

  uint64_t lumCount = 0;
  uint32_t lumHist[256] = {};
  
  // Subpixel sampling: middel meerdere source pixels per scaled pixel
  for (int y = 0; y < scaledH; ++y) {
    int syStart = scratch.yMapStart[y];
    int syEnd = scratch.yMapEnd[y];
    uint32_t* dstRow =
        scratch.scaledRGBA.data() + static_cast<size_t>(y) * scaledW;
    uint16_t* padRow =
        scratch.lumaPad16.data() + static_cast<size_t>(y + 1) * padStride;

    for (int x = 0; x < scaledW; ++x) {
      int sxStart = scratch.xMapStart[x];
      int sxEnd = scratch.xMapEnd[x];
      
      // Accumuleer alle source pixels in dit bereik
      uint32_t sumR = 0, sumG = 0, sumB = 0, sumA = 0;
      uint32_t sumLum = 0;
      int sampleCount = 0;
      
      for (int sy = syStart; sy < syEnd; ++sy) {
        const uint8_t* srcRow = rgba + static_cast<size_t>(sy) * width * 4;
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
          // Luminantie per sample voor precisie
          uint32_t lum32 = kYFromR[r] + kYFromG[g] + kYFromB[b];
          sumLum += (lum32 + 128) >> 8;
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
        
        uint16_t avgLum = static_cast<uint16_t>(sumLum / sampleCount);
        if (avgLum > 255) avgLum = 255;
        
        bool countLum = true;
        if constexpr (!AssumeOpaque) {
          if (a == 0) {
            avgLum = 255;
            countLum = false;
          }
        }
        padRow[x + 1] = avgLum;
        if (countLum) {
          ++lumHist[avgLum];
          ++lumCount;
        }
      } else {
        dstRow[x] = packRGBA(0, 0, 0, 0);
        padRow[x + 1] = 255;
      }
    }
    padRow[0] = padRow[1];
    padRow[padStride - 1] = padRow[padStride - 2];
  }

  std::memcpy(scratch.lumaPad16.data(),
              scratch.lumaPad16.data() + padStride,
              static_cast<size_t>(padStride) * sizeof(uint16_t));
  std::memcpy(scratch.lumaPad16.data() +
                  static_cast<size_t>(scaledH + 1) * padStride,
              scratch.lumaPad16.data() +
                  static_cast<size_t>(scaledH) * padStride,
              static_cast<size_t>(padStride) * sizeof(uint16_t));

  if constexpr (kUseEdgeBoost) {
    for (int y = 0; y < scaledH; ++y) {
      const uint16_t* RESTRICT row =
          scratch.lumaPad16.data() + static_cast<size_t>(y + 1) * padStride + 1;
      uint8_t* RESTRICT edgeRow =
          scratch.edgeMap.data() + static_cast<size_t>(y) * scaledW;
      uint8_t* RESTRICT contrastRow =
          scratch.localContrast.data() + static_cast<size_t>(y) * scaledW;
      for (int x = 0; x < scaledW; ++x) {
        const uint16_t* p = row + x;
        // Sobel kernel met 16-bit precisie
        int a00 = static_cast<int>(p[-padStride - 1]);
        int a01 = static_cast<int>(p[-padStride]);
        int a02 = static_cast<int>(p[-padStride + 1]);
        int a10 = static_cast<int>(p[-1]);
        int a11 = static_cast<int>(p[0]);
        int a12 = static_cast<int>(p[1]);
        int a20 = static_cast<int>(p[padStride - 1]);
        int a21 = static_cast<int>(p[padStride]);
        int a22 = static_cast<int>(p[padStride + 1]);
        
        // Sobel gradiënt
        int gx = (a02 + (a12 << 1) + a22) - (a00 + (a10 << 1) + a20);
        int gy = (a20 + (a21 << 1) + a22) - (a00 + (a01 << 1) + a02);
        int magSq = gx * gx + gy * gy;
        int mag = fastIntSqrt(magSq);
        int edge = mag >> (kEdgeShift - 1);
        if (edge > 255) edge = 255;
        edgeRow[x] = static_cast<uint8_t>(edge);
        
        // Lokaal contrast: verschil tussen center en gemiddelde neighbours
        int avgNeighbor = (a00 + a01 + a02 + a10 + a12 + a20 + a21 + a22) >> 3;
        int localDiff = std::abs(a11 - avgNeighbor);
        if (localDiff > 255) localDiff = 255;
        contrastRow[x] = static_cast<uint8_t>(localDiff);
      }
    }
  }

  int lumLow = 0;
  int lumHigh = 255;
  if (lumCount > 0) {
    uint64_t lowTarget = static_cast<uint64_t>(
        std::max(1.0, std::round(static_cast<double>(lumCount) *
                                 kLumLowPercent)));
    uint64_t highTarget = static_cast<uint64_t>(
        std::max(1.0, std::round(static_cast<double>(lumCount) *
                                 kLumHighPercent)));
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

  const int lumRange = std::max(1, lumHigh - lumLow);

  const int brailleMap[2][4] = {{0, 1, 2, 6}, {3, 4, 5, 7}};

  for (int cy = 0; cy < outH; ++cy) {
    int baseY = cy * 4;
    // Prefetch volgende rij data voor betere cache hits
    if (cy + 1 < outH) {
      int nextBaseY = (cy + 1) * 4;
      _mm_prefetch(reinterpret_cast<const char*>(
          scratch.scaledRGBA.data() + static_cast<size_t>(nextBaseY) * scaledW),
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
      uint16_t lumVals[8];  // 16-bit voor meer precisie
      uint8_t edgeVals[8];
      uint8_t contrastVals[8];
      uint8_t bitIds[8];
      uint8_t validVals[8];
      int dotIndex = 0;

      // Verzamel lokale statistieken voor adaptive thresholding
      int cellLumMin = 255, cellLumMax = 0;
      int cellLumSum = 0;
      int validCount = 0;

      for (int dy = 0; dy < 4; ++dy) {
        int y = baseY + dy;
        const uint32_t* rgbRow =
            scratch.scaledRGBA.data() + static_cast<size_t>(y) * scaledW +
            baseX;
        const uint16_t* lumRow =
            scratch.lumaPad16.data() + static_cast<size_t>(y + 1) * padStride +
            (baseX + 1);
        const uint8_t* edgeRow = scratch.edgeMap.data() +
                                 static_cast<size_t>(y) * scaledW + baseX;
        const uint8_t* contrastRow = scratch.localContrast.data() +
                                     static_cast<size_t>(y) * scaledW + baseX;

        for (int dx = 0; dx < 2; ++dx) {
          uint32_t px = rgbRow[dx];
          uint8_t r = static_cast<uint8_t>(px & 0xFF);
          uint8_t g = static_cast<uint8_t>((px >> 8) & 0xFF);
          uint8_t b = static_cast<uint8_t>((px >> 16) & 0xFF);
          uint16_t lum = lumRow[dx];
          uint8_t a = 255;
          if constexpr (!AssumeOpaque) {
            a = static_cast<uint8_t>((px >> 24) & 0xFF);
          }
          rVals[dotIndex] = r;
          gVals[dotIndex] = g;
          bVals[dotIndex] = b;
          lumVals[dotIndex] = lum;
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
          int lumInt = static_cast<int>(lum);
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
      bool useLocalThreshold = cellLumRange > 20;  // Alleen bij voldoende lokaal contrast
      int localMidpoint = useLocalThreshold ? ((cellLumMin + cellLumMax) >> 1) : bgLum;

      // Sorteer dots op luminantie voor rank-based thresholding
      struct DotInfo { int idx; int score; };
      DotInfo dotRanks[8];
      int numValidDots = 0;
      
      for (int i = 0; i < 8; ++i) {
        if (!validVals[i]) continue;
        
        int lum = static_cast<int>(lumVals[i]);
        int edge = static_cast<int>(edgeVals[i]);
        int contrast = static_cast<int>(contrastVals[i]);
        
        // Score combineert luminantie verschil + edge boost + lokaal contrast
        int lumDiff = useLocalThreshold 
            ? std::abs(lum - localMidpoint)
            : std::abs(lum - bgLum);
        
        int score = lumDiff * 2 + edge + (contrast >> 1);
        dotRanks[numValidDots++] = {i, score};
      }

      // Bepaal hoeveel dots we moeten aanzetten gebaseerd op gemiddelde coverage
      int avgLumDiff = validCount > 0 
          ? std::abs(cellLumMean - bgLum) * 255 / std::max(1, lumRange) 
          : 0;
      if (avgLumDiff > 255) avgLumDiff = 255;
      int targetCoverage = kInkLevelFromLum[static_cast<size_t>(avgLumDiff)];
      int targetDots = (numValidDots * targetCoverage + 127) / 255;
      
      // Extra dots voor hoog-contrast cellen
      if (cellLumRange > 40 && targetDots < numValidDots) {
        targetDots = std::min(targetDots + 1, numValidDots);
      }

      // Sorteer dots op score (hoogste eerst)
      for (int i = 0; i < numValidDots - 1; ++i) {
        for (int j = i + 1; j < numValidDots; ++j) {
          if (dotRanks[j].score > dotRanks[i].score) {
            DotInfo tmp = dotRanks[i];
            dotRanks[i] = dotRanks[j];
            dotRanks[j] = tmp;
          }
        }
      }

      // Activeer de top N dots, met dither voor de grensgevallen
      for (int rank = 0; rank < numValidDots; ++rank) {
        int i = dotRanks[rank].idx;
        int edge = static_cast<int>(edgeVals[i]);
        
        // Basis threshold gebaseerd op rank
        bool shouldActivate = false;
        if (rank < targetDots) {
          // Zeker aan
          shouldActivate = true;
        } else if (rank == targetDots && targetCoverage > 0) {
          // Grens-dot: gebruik dithering alleen als er coverage is
          int ditherThresh = static_cast<int>(
              kDitherThresholdByBit[static_cast<size_t>(bitIds[i])]);
          int fractionalCoverage = (numValidDots * targetCoverage) % 255;
          shouldActivate = fractionalCoverage > ditherThresh;
        }
        
        // Edge boost kan extra dots aanzetten, maar alleen bij significante edges
        if (!shouldActivate && edge > kEdgeMin * 3) {
          int edgeBonus = kEdgeBoostFromMag[static_cast<size_t>(edge)];
          int ditherThresh = static_cast<int>(
              kDitherThresholdByBit[static_cast<size_t>(bitIds[i])]) - kDitherBias;
          if (ditherThresh < 0) ditherThresh = 0;
          shouldActivate = edgeBonus > ditherThresh;
        }
        
        if (shouldActivate) {
          bitmask |= (1 << bitIds[i]);
        }
      }

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
        uint8_t curR = static_cast<uint8_t>(
            (inkCount > 0 ? sumInkR : sumAllR) /
            (inkCount > 0 ? inkCount : colorCount));
        uint8_t curG = static_cast<uint8_t>(
            (inkCount > 0 ? sumInkG : sumAllG) /
            (inkCount > 0 ? inkCount : colorCount));
        uint8_t curB = static_cast<uint8_t>(
            (inkCount > 0 ? sumInkB : sumAllB) /
            (inkCount > 0 ? inkCount : colorCount));
        curR = static_cast<uint8_t>(std::max<int>(curR, kColorLift));
        curG = static_cast<uint8_t>(std::max<int>(curG, kColorLift));
        curB = static_cast<uint8_t>(std::max<int>(curB, kColorLift));
        int curY =
            (static_cast<int>(curR) * 54 +
             static_cast<int>(curG) * 183 +
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
            curR = static_cast<uint8_t>(
                std::min(255, (static_cast<int>(curR) * scale + 128) >> 8));
            curG = static_cast<uint8_t>(
                std::min(255, (static_cast<int>(curG) * scale + 128) >> 8));
            curB = static_cast<uint8_t>(
                std::min(255, (static_cast<int>(curB) * scale + 128) >> 8));
          }
        }
        // Verbeterde kleurverwerking met adaptive saturation
        int y = (static_cast<int>(curR) * 54 +
                 static_cast<int>(curG) * 183 +
                 static_cast<int>(curB) * 19 + 128) >>
                8;
        // Adaptive saturation: meer saturatie voor donkere kleuren
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
            pr = pr + (((int)curR - pr) * kColorAlpha >> 8);
            pg = pg + (((int)curG - pg) * kColorAlpha >> 8);
            pb = pb + (((int)curB - pb) * kColorAlpha >> 8);
          }
          outR = static_cast<uint8_t>(pr);
          outG = static_cast<uint8_t>(pg);
          outB = static_cast<uint8_t>(pb);
          scratch.prevFg[cellIndex] =
              (static_cast<uint32_t>(pr) << 16) |
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

        uint8_t bgR = static_cast<uint8_t>(
            (bgCount > 0 ? sumBgR : sumAllR) /
            (bgCount > 0 ? bgCount : colorCount));
        uint8_t bgG = static_cast<uint8_t>(
            (bgCount > 0 ? sumBgG : sumAllG) /
            (bgCount > 0 ? bgCount : colorCount));
        uint8_t bgB = static_cast<uint8_t>(
            (bgCount > 0 ? sumBgB : sumAllB) /
            (bgCount > 0 ? bgCount : colorCount));
        int bgY = (static_cast<int>(bgR) * 54 +
                   static_cast<int>(bgG) * 183 +
                   static_cast<int>(bgB) * 19 + 128) >>
                  8;
        if (bgY < kBgMinLuma) {
          if (bgY <= 0) {
            bgR = kBgMinLuma;
            bgG = kBgMinLuma;
            bgB = kBgMinLuma;
          } else {
            int scale = (static_cast<int>(kBgMinLuma) * 256) / bgY;
            bgR = static_cast<uint8_t>(
                std::min(255, (static_cast<int>(bgR) * scale + 128) >> 8));
            bgG = static_cast<uint8_t>(
                std::min(255, (static_cast<int>(bgG) * scale + 128) >> 8));
            bgB = static_cast<uint8_t>(
                std::min(255, (static_cast<int>(bgB) * scale + 128) >> 8));
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
            pr = pr + (((int)bgR - pr) * kBgAlpha >> 8);
            pg = pg + (((int)bgG - pg) * kBgAlpha >> 8);
            pb = pb + (((int)bgB - pb) * kBgAlpha >> 8);
          }
          outBgR = static_cast<uint8_t>(pr);
          outBgG = static_cast<uint8_t>(pg);
          outBgB = static_cast<uint8_t>(pb);
          scratch.prevBg[cellIndex] =
              (static_cast<uint32_t>(pr) << 16) |
              (static_cast<uint32_t>(pg) << 8) |
              static_cast<uint32_t>(pb);
        } else if (cellIndex < scratch.prevBg.size()) {
          outBgR = bgR;
          outBgG = bgG;
          outBgB = bgB;
          scratch.prevBg[cellIndex] =
              (static_cast<uint32_t>(bgR) << 16) |
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
      // Braille geeft de beste resultaten voor detail - gebruik het altijd
      // De hybrid mode is optioneel voor gebieden zonder contrast
      wchar_t finalChar = static_cast<wchar_t>(kBrailleBase + bitmask);
      
      if constexpr (kUseHybridMode) {
        // Alleen voor zeer uniforme gebieden: gebruik density-based ASCII
        if (cellLumRange < kMinContrastForBraille) {
          // Tel dots voor density lookup (O(1) met popcount)
          int dotCount = 0;
          int b = bitmask;
          while (b) { dotCount += (b & 1); b >>= 1; }
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

  return true;
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
