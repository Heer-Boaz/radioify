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
#include <vector>

namespace {
constexpr uint32_t kBrailleBase = 0x2800;

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

float clampFloat(float v, float lo, float hi) {
  return (v < lo) ? lo : (v > hi ? hi : v);
}

constexpr bool kInkUseBright = true;
constexpr float kInkGamma = 0.55f;
constexpr float kCoverageGain = 1.7f;
constexpr float kCoverageBias = 0.16f;
constexpr float kCoverageZeroCutoff = 0.015f;
constexpr float kLumLowPercent = 0.015f;
constexpr float kLumHighPercent = 0.985f;
constexpr uint8_t kColorLift = 0;
constexpr uint8_t kInkMinLuma = 120;
constexpr uint8_t kBgMinLuma = 24;
constexpr int kInkMaxScale = 1024;
constexpr int kColorAlpha = 64;
constexpr int kBgAlpha = 32;
constexpr int kTemporalResetDelta = 60;
constexpr int kColorSaturation = 320;
constexpr bool kUseEdgeBoost = true;
constexpr uint8_t kEdgeMin = 5;
constexpr uint8_t kEdgeBoost = 230;
constexpr int kEdgeShift = 3;
constexpr int kBgDelta = 8;
constexpr int kBgDeltaMin = 2;
constexpr int kEdgeDeltaScale = 80;
constexpr int kDitherBias = 64;
constexpr int kLumSmoothAlpha = 48;
constexpr int kLumResetDelta = 32;

const std::array<uint16_t, 256> kYFromR = []() {
  std::array<uint16_t, 256> t{};
  for (int i = 0; i < 256; ++i) {
    float v = 255.0f * (0.2126f * kSrgbToLinear[i]);
    int r = static_cast<int>(std::lround(v));
    t[i] = static_cast<uint16_t>(std::clamp(r, 0, 255));
  }
  return t;
}();

const std::array<uint16_t, 256> kYFromG = []() {
  std::array<uint16_t, 256> t{};
  for (int i = 0; i < 256; ++i) {
    float v = 255.0f * (0.7152f * kSrgbToLinear[i]);
    int r = static_cast<int>(std::lround(v));
    t[i] = static_cast<uint16_t>(std::clamp(r, 0, 255));
  }
  return t;
}();

const std::array<uint16_t, 256> kYFromB = []() {
  std::array<uint16_t, 256> t{};
  for (int i = 0; i < 256; ++i) {
    float v = 255.0f * (0.0722f * kSrgbToLinear[i]);
    int r = static_cast<int>(std::lround(v));
    t[i] = static_cast<uint16_t>(std::clamp(r, 0, 255));
  }
  return t;
}();

const std::array<uint8_t, 256> kInkLevelFromLum = []() {
  std::array<uint8_t, 256> lut{};
  for (int i = 0; i < 256; ++i) {
    float norm = static_cast<float>(i) / 255.0f;
    float x = kInkUseBright ? norm : (1.0f - norm);
    float coverage = std::pow(x, kInkGamma);
    coverage = coverage * kCoverageGain + kCoverageBias;
    if (coverage < kCoverageZeroCutoff) coverage = 0.0f;
    coverage = std::clamp(coverage, 0.0f, 1.0f);
    lut[static_cast<size_t>(i)] =
        static_cast<uint8_t>(std::lround(coverage * 255.0f));
  }
  return lut;
}();

const std::array<uint8_t, 8> kDitherThresholdByBit = []() {
  std::array<uint8_t, 8> lut{};
  const uint8_t ranks[8] = {0, 6, 3, 4, 2, 7, 5, 1};
  for (int i = 0; i < 8; ++i) {
    lut[static_cast<size_t>(i)] =
        static_cast<uint8_t>(ranks[i] * 32 + 16);
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

inline uint32_t packRGBA(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
  return static_cast<uint32_t>(r) |
         (static_cast<uint32_t>(g) << 8) |
         (static_cast<uint32_t>(b) << 16) |
         (static_cast<uint32_t>(a) << 24);
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

  std::vector<int> xMap;
  std::vector<int> yMap;
  std::vector<uint32_t> scaledRGBA;
  std::vector<uint8_t> lumaPad;
  std::vector<uint8_t> edgeMap;

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

    xMap.resize(static_cast<size_t>(scaledW));
    yMap.resize(static_cast<size_t>(scaledH));
    for (int x = 0; x < scaledW; ++x) {
      xMap[x] = (x * w) / scaledW;
    }
    for (int y = 0; y < scaledH; ++y) {
      yMap[y] = (y * h) / scaledH;
    }

    scaledRGBA.resize(static_cast<size_t>(scaledW) * scaledH);
    lumaPad.resize(static_cast<size_t>(padStride) * (scaledH + 2));
    edgeMap.resize(static_cast<size_t>(scaledW) * scaledH);
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
  for (int y = 0; y < scaledH; ++y) {
    int sy = scratch.yMap[y];
    const uint8_t* srcRow =
        rgba + static_cast<size_t>(sy) * width * 4;
    uint32_t* dstRow =
        scratch.scaledRGBA.data() + static_cast<size_t>(y) * scaledW;
    uint8_t* padRow =
        scratch.lumaPad.data() + static_cast<size_t>(y + 1) * padStride;

    for (int x = 0; x < scaledW; ++x) {
      int sx = scratch.xMap[x];
      const uint8_t* sp = srcRow + sx * 4;

      uint8_t r = sp[0];
      uint8_t g = sp[1];
      uint8_t b = sp[2];
      uint8_t a = 255;
      if constexpr (!AssumeOpaque) {
        a = sp[3];
      }

      dstRow[x] = packRGBA(r, g, b, a);

      uint16_t y16 =
          static_cast<uint16_t>(kYFromR[r] + kYFromG[g] + kYFromB[b]);
      if (y16 > 255) y16 = 255;
      bool countLum = true;
      if constexpr (!AssumeOpaque) {
        if (a == 0) {
          y16 = 255;
          countLum = false;
        }
      }
      padRow[x + 1] = static_cast<uint8_t>(y16);
      if (countLum) {
        ++lumHist[y16];
        ++lumCount;
      }
    }
    padRow[0] = padRow[1];
    padRow[padStride - 1] = padRow[padStride - 2];
  }

  std::memcpy(scratch.lumaPad.data(),
              scratch.lumaPad.data() + padStride,
              static_cast<size_t>(padStride));
  std::memcpy(scratch.lumaPad.data() +
                  static_cast<size_t>(scaledH + 1) * padStride,
              scratch.lumaPad.data() +
                  static_cast<size_t>(scaledH) * padStride,
              static_cast<size_t>(padStride));

  if constexpr (kUseEdgeBoost) {
    for (int y = 0; y < scaledH; ++y) {
      const uint8_t* row =
          scratch.lumaPad.data() + static_cast<size_t>(y + 1) * padStride + 1;
      uint8_t* edgeRow =
          scratch.edgeMap.data() + static_cast<size_t>(y) * scaledW;
      for (int x = 0; x < scaledW; ++x) {
        const uint8_t* p = row + x;
        int a00 = static_cast<int>(p[-padStride - 1]);
        int a01 = static_cast<int>(p[-padStride]);
        int a02 = static_cast<int>(p[-padStride + 1]);
        int a10 = static_cast<int>(p[-1]);
        int a12 = static_cast<int>(p[1]);
        int a20 = static_cast<int>(p[padStride - 1]);
        int a21 = static_cast<int>(p[padStride]);
        int a22 = static_cast<int>(p[padStride + 1]);
        int gx = (a02 + (a12 << 1) + a22) - (a00 + (a10 << 1) + a20);
        int gy = (a20 + (a21 << 1) + a22) - (a00 + (a01 << 1) + a02);
        int mag = std::abs(gx) + std::abs(gy);
        int edge = mag >> kEdgeShift;
        if (edge > 255) edge = 255;
        edgeRow[x] = static_cast<uint8_t>(edge);
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
      uint8_t lumVals[8];
      uint8_t edgeVals[8];
      uint8_t bitIds[8];
      uint8_t validVals[8];
      int dotIndex = 0;

      for (int dy = 0; dy < 4; ++dy) {
        int y = baseY + dy;
        const uint32_t* rgbRow =
            scratch.scaledRGBA.data() + static_cast<size_t>(y) * scaledW +
            baseX;
        const uint8_t* lumRow =
            scratch.lumaPad.data() + static_cast<size_t>(y + 1) * padStride +
            (baseX + 1);
        const uint8_t* edgeRow = scratch.edgeMap.data() +
                                 static_cast<size_t>(y) * scaledW + baseX;

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
          lumVals[dotIndex] = lum;
          edgeVals[dotIndex] = edgeRow[dx];
          bitIds[dotIndex] = static_cast<uint8_t>(brailleMap[dx][dy]);
          validVals[dotIndex] = static_cast<uint8_t>(a != 0);

          if constexpr (!AssumeOpaque) {
            if (a == 0) {
              ++dotIndex;
              continue;
            }
          }

          sumAllR += r;
          sumAllG += g;
          sumAllB += b;
          ++colorCount;
          ++dotIndex;
        }
      }

      for (int i = 0; i < 8; ++i) {
        int edge = static_cast<int>(edgeVals[i]);
        int diffMin =
            kBgDelta - ((edge * kEdgeDeltaScale) >> 8);
        if (diffMin < kBgDeltaMin) diffMin = kBgDeltaMin;
        int lumDiff =
            std::abs(static_cast<int>(lumVals[i]) - bgLum);
        if (lumDiff < diffMin) {
          continue;
        }
        int dotNorm = lumDiff * 255 / lumRange;
        if (dotNorm < 0) dotNorm = 0;
        if (dotNorm > 255) dotNorm = 255;
        uint8_t level = kInkLevelFromLum[static_cast<size_t>(dotNorm)];
        int boosted = static_cast<int>(level) +
                      kEdgeBoostFromMag[static_cast<size_t>(edgeVals[i])];
        if (boosted > 255) boosted = 255;
        int threshold = static_cast<int>(
            kDitherThresholdByBit[static_cast<size_t>(bitIds[i])]);
        threshold -= kDitherBias;
        if (threshold < 0) threshold = 0;
        if (boosted > threshold) {
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
        int y = (static_cast<int>(curR) * 54 +
                 static_cast<int>(curG) * 183 +
                 static_cast<int>(curB) * 19 + 128) >>
                8;
        curR = static_cast<uint8_t>(std::clamp(
            y + ((static_cast<int>(curR) - y) * kColorSaturation >> 8), 0,
            255));
        curG = static_cast<uint8_t>(std::clamp(
            y + ((static_cast<int>(curG) - y) * kColorSaturation >> 8), 0,
            255));
        curB = static_cast<uint8_t>(std::clamp(
            y + ((static_cast<int>(curB) - y) * kColorSaturation >> 8), 0,
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
      cell.ch = static_cast<wchar_t>(kBrailleBase + bitmask);
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
