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

constexpr int kBgDistSq = 32 * 32;
constexpr int kDeltaBase = 30;
constexpr int kDeltaMin = 10;
constexpr float kEdgeCoeff = 0.2f;
constexpr float kBgEmaAlpha = 0.15f;
constexpr uint8_t kFgHoldThreshold = 3;

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

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  uint16_t r5 = static_cast<uint16_t>(r >> 3);
  uint16_t g6 = static_cast<uint16_t>(g >> 2);
  uint16_t b5 = static_cast<uint16_t>(b >> 3);
  return static_cast<uint16_t>((r5 << 11) | (g6 << 5) | b5);
}

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

  std::vector<uint32_t> histCnt;
  std::vector<uint32_t> histSumR;
  std::vector<uint32_t> histSumG;
  std::vector<uint32_t> histSumB;
  std::vector<uint16_t> touchedBins;
  std::vector<uint32_t> prevFgKeys;
  std::vector<uint8_t> prevFgValid;
  float bgRema = 0.0f;
  float bgGema = 0.0f;
  float bgBema = 0.0f;
  bool bgInit = false;

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
    prevFgKeys.assign(static_cast<size_t>(outW) * outH, 0);
    prevFgValid.assign(static_cast<size_t>(outW) * outH, 0);
    bgRema = 0.0f;
    bgGema = 0.0f;
    bgBema = 0.0f;
    bgInit = false;

    if (histCnt.empty()) {
      histCnt.assign(65536u, 0u);
      histSumR.assign(65536u, 0u);
      histSumG.assign(65536u, 0u);
      histSumB.assign(65536u, 0u);
      touchedBins.reserve(4096);
    }
  }

  void beginFrame() {
    for (uint16_t bin : touchedBins) {
      histCnt[bin] = 0;
      histSumR[bin] = 0;
      histSumG[bin] = 0;
      histSumB[bin] = 0;
    }
    touchedBins.clear();
  }
};

inline int iabs(int v) { return v < 0 ? -v : v; }

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

  scratch.beginFrame();

  const int outW = scratch.outW;
  const int outH = scratch.outH;
  const int scaledW = scratch.scaledW;
  const int scaledH = scratch.scaledH;
  const int padStride = scratch.padStride;

  out.width = outW;
  out.height = outH;
  out.cells.resize(static_cast<size_t>(outW) * outH);

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
      padRow[x + 1] = static_cast<uint8_t>(y16);

      if constexpr (AssumeOpaque) {
        uint16_t bin = rgb565(r, g, b);
        uint32_t& c = scratch.histCnt[bin];
        if (c == 0) scratch.touchedBins.push_back(bin);
        ++c;
        scratch.histSumR[bin] += r;
        scratch.histSumG[bin] += g;
        scratch.histSumB[bin] += b;
      } else if (a != 0) {
        uint16_t bin = rgb565(r, g, b);
        uint32_t& c = scratch.histCnt[bin];
        if (c == 0) scratch.touchedBins.push_back(bin);
        ++c;
        scratch.histSumR[bin] += r;
        scratch.histSumG[bin] += g;
        scratch.histSumB[bin] += b;
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

  uint32_t bestCount = 0;
  uint16_t bestBin = 0;
  for (uint16_t bin : scratch.touchedBins) {
    uint32_t c = scratch.histCnt[bin];
    if (c > bestCount) {
      bestCount = c;
      bestBin = bin;
    }
  }

  uint8_t bgR = 0;
  uint8_t bgG = 0;
  uint8_t bgB = 0;
  if (bestCount != 0) {
    bgR = static_cast<uint8_t>(scratch.histSumR[bestBin] / bestCount);
    bgG = static_cast<uint8_t>(scratch.histSumG[bestBin] / bestCount);
    bgB = static_cast<uint8_t>(scratch.histSumB[bestBin] / bestCount);
  }
  if (!scratch.bgInit) {
    scratch.bgRema = bgR;
    scratch.bgGema = bgG;
    scratch.bgBema = bgB;
    scratch.bgInit = true;
  } else {
    scratch.bgRema += kBgEmaAlpha * (bgR - scratch.bgRema);
    scratch.bgGema += kBgEmaAlpha * (bgG - scratch.bgGema);
    scratch.bgBema += kBgEmaAlpha * (bgB - scratch.bgBema);
  }
  bgR = static_cast<uint8_t>(
      std::lround(std::clamp(scratch.bgRema, 0.0f, 255.0f)));
  bgG = static_cast<uint8_t>(
      std::lround(std::clamp(scratch.bgGema, 0.0f, 255.0f)));
  bgB = static_cast<uint8_t>(
      std::lround(std::clamp(scratch.bgBema, 0.0f, 255.0f)));

  uint16_t bgLum16 =
      static_cast<uint16_t>(kYFromR[bgR] + kYFromG[bgG] + kYFromB[bgB]);
  if (bgLum16 > 255) bgLum16 = 255;
  int bgLum = static_cast<int>(bgLum16);

  const int brailleMap[2][4] = {{0, 1, 2, 6}, {3, 4, 5, 7}};

  for (int cy = 0; cy < outH; ++cy) {
    int baseY = cy * 4;
    for (int cx = 0; cx < outW; ++cx) {
      int baseX = cx * 2;
      int bitmask = 0;

      uint32_t keys[8];
      uint8_t counts[8];
      int uniq = 0;

      int bgSumR = 0;
      int bgSumG = 0;
      int bgSumB = 0;
      int bgCnt = 0;

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
              bgSumR += r;
              bgSumG += g;
              bgSumB += b;
              ++bgCnt;
              continue;
            }
          }

          int dr = static_cast<int>(r) - static_cast<int>(bgR);
          int dg = static_cast<int>(g) - static_cast<int>(bgG);
          int db = static_cast<int>(b) - static_cast<int>(bgB);
          int dist = dr * dr + dg * dg + db * db;
          if (dist < kBgDistSq) {
            bgSumR += r;
            bgSumG += g;
            bgSumB += b;
            ++bgCnt;
            continue;
          }

          uint8_t lum = lumRow[dx];
          int lumDiff = iabs(static_cast<int>(lum) - bgLum);

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

          if (lumDiff >= static_cast<int>(deltaThr)) {
            bitmask |= (1 << brailleMap[dx][dy]);

            uint32_t key = (static_cast<uint32_t>(r) << 16) |
                           (static_cast<uint32_t>(g) << 8) |
                           static_cast<uint32_t>(b);
            int found = -1;
            for (int i = 0; i < uniq; ++i) {
              if (keys[i] == key) {
                found = i;
                break;
              }
            }
            if (found >= 0) {
              ++counts[found];
            } else if (uniq < 8) {
              keys[uniq] = key;
              counts[uniq] = 1;
              ++uniq;
            }
          }
        }
      }

      uint32_t domKey = 0x808080;
      uint8_t domCnt = 0;
      for (int i = 0; i < uniq; ++i) {
        if (counts[i] > domCnt) {
          domCnt = counts[i];
          domKey = keys[i];
        }
      }

      size_t cellIndex = static_cast<size_t>(cy) * outW + cx;
      uint32_t finalKey = domKey;
      if (cellIndex < scratch.prevFgKeys.size()) {
        if (scratch.prevFgValid[cellIndex] && domCnt < kFgHoldThreshold) {
          finalKey = scratch.prevFgKeys[cellIndex];
        } else {
          scratch.prevFgKeys[cellIndex] = domKey;
          scratch.prevFgValid[cellIndex] = 1;
        }
      }

      AsciiArt::AsciiCell cell{};
      cell.ch = static_cast<wchar_t>(kBrailleBase + bitmask);
      cell.fg = Color{static_cast<uint8_t>((finalKey >> 16) & 0xFF),
                      static_cast<uint8_t>((finalKey >> 8) & 0xFF),
                      static_cast<uint8_t>(finalKey & 0xFF)};
      if (bgCnt > 0) {
        cell.hasBg = true;
        cell.bg = Color{static_cast<uint8_t>(bgSumR / bgCnt),
                        static_cast<uint8_t>(bgSumG / bgCnt),
                        static_cast<uint8_t>(bgSumB / bgCnt)};
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
