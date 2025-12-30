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

constexpr int kDeltaBase = 30;
constexpr int kDeltaMin = 10;
constexpr float kEdgeCoeff = 0.2f;
constexpr float kLumEmaAlpha = 0.15f;
constexpr float kLumBias = 0.0f;
constexpr float kLumMin = 20.0f;
constexpr float kLumMax = 235.0f;
constexpr float kDotRatioDark = 0.55f;
constexpr float kDotRatioBright = 0.90f;
constexpr float kDotRatioMinMean = 48.0f;
constexpr float kDotRatioMaxMean = 210.0f;
constexpr uint8_t kColorHoldThreshold = 2;
constexpr uint8_t kColorLift = 0;
constexpr uint8_t kInkMinLuma = 90;
constexpr int kInkMaxScale = 1024;
constexpr int kColorAlpha = 64;
constexpr int kColorSaturation = 320;

constexpr int kMaxSobel = 1020;
constexpr int kMaxG2 = 2 * kMaxSobel * kMaxSobel;
constexpr int kEdgeShift = 4;
constexpr int kDeltaLutSize = (kMaxG2 >> kEdgeShift) + 1;

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

const std::array<uint8_t, kDeltaLutSize> kDeltaThrFromG2 = []() {
  std::array<uint8_t, kDeltaLutSize> lut{};
  for (int i = 0; i < kDeltaLutSize; ++i) {
    int g2 = (i << kEdgeShift);
    float edge = std::sqrt(static_cast<float>(g2));
    float thr = static_cast<float>(kDeltaBase) - kEdgeCoeff * edge;
    if (thr < static_cast<float>(kDeltaMin)) thr = static_cast<float>(kDeltaMin);
    if (thr > 255.0f) thr = 255.0f;
    lut[static_cast<size_t>(i)] =
        static_cast<uint8_t>(std::lround(thr));
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

  std::vector<uint32_t> prevFg;
  std::vector<uint8_t> prevFgValid;
  float thrLumEma = 0.0f;
  bool thrLumInit = false;

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
    prevFg.assign(static_cast<size_t>(outW) * outH, 0);
    prevFgValid.assign(static_cast<size_t>(outW) * outH, 0);
    thrLumEma = 0.0f;
    thrLumInit = false;
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

  float thrLum = scratch.thrLumInit ? scratch.thrLumEma : kLumMin;
  if (lumCount > 0) {
    uint64_t sumLum = 0;
    for (int i = 0; i < 256; ++i) {
      sumLum += static_cast<uint64_t>(i) * lumHist[i];
    }
    float meanLum =
        static_cast<float>(sumLum) / static_cast<float>(lumCount);
    float t = 0.0f;
    if (kDotRatioMaxMean > kDotRatioMinMean) {
      t = (meanLum - kDotRatioMinMean) /
          (kDotRatioMaxMean - kDotRatioMinMean);
    }
    t = std::clamp(t, 0.0f, 1.0f);
    float dotRatio =
        kDotRatioDark + (kDotRatioBright - kDotRatioDark) * t;
    uint64_t target = static_cast<uint64_t>(
        std::max(1.0, std::round(static_cast<double>(lumCount) * dotRatio)));
    uint64_t accum = 0;
    int thrFromHist = 255;
    for (int i = 255; i >= 0; --i) {
      accum += lumHist[i];
      if (accum >= target) {
        thrFromHist = i;
        break;
      }
    }
    thrLum = std::clamp(static_cast<float>(thrFromHist) - kLumBias, kLumMin,
                        kLumMax);
    if (!scratch.thrLumInit) {
      scratch.thrLumEma = thrLum;
      scratch.thrLumInit = true;
    } else {
      scratch.thrLumEma += kLumEmaAlpha * (thrLum - scratch.thrLumEma);
      thrLum = scratch.thrLumEma;
    }
  }
  int thrLumInt = static_cast<int>(
      std::lround(std::clamp(scratch.thrLumEma, 0.0f, 255.0f)));

  const int brailleMap[2][4] = {{0, 1, 2, 6}, {3, 4, 5, 7}};

  for (int cy = 0; cy < outH; ++cy) {
    int baseY = cy * 4;
    for (int cx = 0; cx < outW; ++cx) {
      int baseX = cx * 2;
      int bitmask = 0;

      int dotCount = 0;
      int sumR = 0;
      int sumG = 0;
      int sumB = 0;

      for (int dy = 0; dy < 4; ++dy) {
        int y = baseY + dy;
        const uint32_t* rgbRow =
            scratch.scaledRGBA.data() + static_cast<size_t>(y) * scaledW +
            baseX;
        const uint8_t* lumRow =
            scratch.lumaPad.data() + static_cast<size_t>(y + 1) * padStride +
            (baseX + 1);

        for (int dx = 0; dx < 2; ++dx) {
          uint32_t px = rgbRow[dx];
          uint8_t r = static_cast<uint8_t>(px & 0xFF);
          uint8_t g = static_cast<uint8_t>((px >> 8) & 0xFF);
          uint8_t b = static_cast<uint8_t>((px >> 16) & 0xFF);
          if constexpr (!AssumeOpaque) {
            uint8_t a = static_cast<uint8_t>((px >> 24) & 0xFF);
            if (a == 0) {
              continue;
            }
          }

          uint8_t lum = lumRow[dx];

          const uint8_t* p = lumRow + dx;
          int tl = p[-padStride - 1];
          int tc = p[-padStride];
          int tr = p[-padStride + 1];
          int ml = p[-1];
          int mr = p[1];
          int bl = p[padStride - 1];
          int bc = p[padStride];
          int br = p[padStride + 1];

          int gx = (tr + (mr << 1) + br) - (tl + (ml << 1) + bl);
          int gy = (bl + (bc << 1) + br) - (tl + (tc << 1) + tr);
          int g2 = gx * gx + gy * gy;

          uint8_t deltaThr =
              kDeltaThrFromG2[static_cast<size_t>(g2 >> kEdgeShift)];

          int edgeBias = static_cast<int>(kDeltaBase) -
                         static_cast<int>(deltaThr);
          if (edgeBias < 0) edgeBias = 0;
          int threshold = thrLumInt - edgeBias;
          if (threshold < 0) threshold = 0;
          if (threshold > 255) threshold = 255;
          if (lum > static_cast<uint8_t>(threshold)) {
            bitmask |= (1 << brailleMap[dx][dy]);
            ++dotCount;
            sumR += r;
            sumG += g;
            sumB += b;
          }
        }
      }

      size_t cellIndex = static_cast<size_t>(cy) * outW + cx;
      uint8_t outR = 0;
      uint8_t outG = 0;
      uint8_t outB = 0;
      if (dotCount > 0) {
        uint8_t curR = static_cast<uint8_t>(sumR / dotCount);
        uint8_t curG = static_cast<uint8_t>(sumG / dotCount);
        uint8_t curB = static_cast<uint8_t>(sumB / dotCount);
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
            scratch.prevFgValid[cellIndex] &&
            dotCount <= kColorHoldThreshold) {
          uint32_t p = scratch.prevFg[cellIndex];
          outR = static_cast<uint8_t>((p >> 16) & 0xFF);
          outG = static_cast<uint8_t>((p >> 8) & 0xFF);
          outB = static_cast<uint8_t>(p & 0xFF);
        } else if (cellIndex < scratch.prevFg.size() &&
                   scratch.prevFgValid[cellIndex]) {
          uint32_t p = scratch.prevFg[cellIndex];
          int pr = static_cast<int>((p >> 16) & 0xFF);
          int pg = static_cast<int>((p >> 8) & 0xFF);
          int pb = static_cast<int>(p & 0xFF);
          pr = pr + (((int)curR - pr) * kColorAlpha >> 8);
          pg = pg + (((int)curG - pg) * kColorAlpha >> 8);
          pb = pb + (((int)curB - pb) * kColorAlpha >> 8);
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
      } else if (cellIndex < scratch.prevFg.size() &&
                 scratch.prevFgValid[cellIndex]) {
        uint32_t p = scratch.prevFg[cellIndex];
        outR = static_cast<uint8_t>((p >> 16) & 0xFF);
        outG = static_cast<uint8_t>((p >> 8) & 0xFF);
        outB = static_cast<uint8_t>(p & 0xFF);
      }

      AsciiArt::AsciiCell cell{};
      cell.ch = static_cast<wchar_t>(kBrailleBase + bitmask);
      cell.fg = Color{outR, outG, outB};

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
