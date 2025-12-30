#include "asciiart.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace {
constexpr uint32_t kBrailleBase = 0x2800;

const std::array<float, 256> kSrgbToLinear = []() {
  std::array<float, 256> table{};
  for (int i = 0; i < 256; ++i) {
    float s = static_cast<float>(i) / 255.0f;
    table[i] = (s <= 0.04045f) ? (s / 12.92f)
                               : std::pow((s + 0.055f) / 1.055f, 2.4f);
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

uint32_t rgbToKey(uint8_t r, uint8_t g, uint8_t b) {
  return (static_cast<uint32_t>(r) << 16) |
         (static_cast<uint32_t>(g) << 8) | static_cast<uint32_t>(b);
}

float clampFloat(float v, float lo, float hi) {
  return (v < lo) ? lo : (v > hi ? hi : v);
}

uint8_t clampByte(float v) {
  long rounded = std::lround(v);
  if (rounded < 0) return 0;
  if (rounded > 255) return 255;
  return static_cast<uint8_t>(rounded);
}

float srgb2lin(uint8_t v) { return kSrgbToLinear[v]; }

float colorDistSq(uint8_t r1, uint8_t g1, uint8_t b1, uint8_t r2, uint8_t g2,
                  uint8_t b2) {
  int dr = static_cast<int>(r1) - static_cast<int>(r2);
  int dg = static_cast<int>(g1) - static_cast<int>(g2);
  int db = static_cast<int>(b1) - static_cast<int>(b2);
  return static_cast<float>(dr * dr + dg * dg + db * db);
}

float sobelAt(const std::vector<float>& buf, int w, int h, int x, int y) {
  int xm1 = std::max(0, x - 1);
  int xp1 = std::min(w - 1, x + 1);
  int ym1 = std::max(0, y - 1);
  int yp1 = std::min(h - 1, y + 1);
  int row = y * w;
  float gx = buf[ym1 * w + xp1] + 2.0f * buf[row + xp1] +
             buf[yp1 * w + xp1] - buf[ym1 * w + xm1] -
             2.0f * buf[row + xm1] - buf[yp1 * w + xm1];
  float gy = buf[yp1 * w + xm1] + 2.0f * buf[yp1 * w + x] +
             buf[yp1 * w + xp1] - buf[ym1 * w + xm1] -
             2.0f * buf[ym1 * w + x] - buf[ym1 * w + xp1];
  return std::hypot(gx, gy);
}

void distributeError(std::vector<float>& buf, float e, int idx, int w, int h) {
  int x = idx % w;
  int y = idx / w;
  if (x + 1 < w) buf[idx + 1] += e * 7.0f / 16.0f;
  if (x > 0 && y + 1 < h) buf[idx + w - 1] += e * 3.0f / 16.0f;
  if (y + 1 < h) buf[idx + w] += e * 5.0f / 16.0f;
  if (x + 1 < w && y + 1 < h) buf[idx + w + 1] += e * 1.0f / 16.0f;
}

bool loadImagePixels(const std::filesystem::path& path, int& outW, int& outH,
                     std::vector<uint8_t>& pixels, std::string* error) {
  ComScope com(error);
  if (!com.ok()) return false;

  IWICImagingFactory* factory = nullptr;
  HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    setError(error, "Failed to create image decoder.");
    return false;
  }

  IWICBitmapDecoder* decoder = nullptr;
  std::wstring wpath = path.wstring();
  hr = factory->CreateDecoderFromFilename(
      wpath.c_str(), nullptr, GENERIC_READ,
      WICDecodeMetadataCacheOnDemand, &decoder);
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

std::vector<uint8_t> downscaleImageNN(const std::vector<uint8_t>& src, int srcW,
                                      int srcH, int dstW, int dstH) {
  std::vector<uint8_t> dst(static_cast<size_t>(dstW * dstH * 4));
  for (int y = 0; y < dstH; ++y) {
    int sy = static_cast<int>(y * srcH / dstH);
    for (int x = 0; x < dstW; ++x) {
      int sx = static_cast<int>(x * srcW / dstW);
      int srcIdx = (sy * srcW + sx) * 4;
      int dstIdx = (y * dstW + x) * 4;
      dst[dstIdx] = src[static_cast<size_t>(srcIdx)];
      dst[dstIdx + 1] = src[static_cast<size_t>(srcIdx + 1)];
      dst[dstIdx + 2] = src[static_cast<size_t>(srcIdx + 2)];
      dst[dstIdx + 3] = src[static_cast<size_t>(srcIdx + 3)];
    }
  }
  return dst;
}

AsciiArt generateBrailleArt(const std::vector<uint8_t>& imgBuf, int imgW,
                            int imgH, int maxArtWidth) {
  AsciiArt art;
  int maxOutW = std::max(1, maxArtWidth - 8);
  float scale = 1.0f;
  if (static_cast<int>(std::floor(imgW / 2.0f)) > maxOutW) {
    scale = maxOutW / std::floor(imgW / 2.0f);
  }

  std::vector<uint8_t> scaledBuf = imgBuf;
  int scaledW = imgW;
  int scaledH = imgH;
  if (scale < 1.0f) {
    scaledW = std::max(2, static_cast<int>(std::floor(imgW * scale)));
    scaledH = std::max(4, static_cast<int>(std::floor(imgH * scale)));
    scaledBuf = downscaleImageNN(imgBuf, imgW, imgH, scaledW, scaledH);
  }

  const bool useEdge = true;
  const bool useDith = false;
  const float bgDist = 32.0f * 32.0f;
  const float delta = 30.0f;

  const int brailleMap[2][4] = {{0, 1, 2, 6}, {3, 4, 5, 7}};
  int outW = std::min(maxOutW, static_cast<int>(std::floor(scaledW / 2.0f)));
  outW = std::max(1, outW);
  int outH =
      std::max(static_cast<int>(std::ceil(scaledH / 4.0f)),
               static_cast<int>(std::floor(
                   outW * (static_cast<float>(scaledH) / scaledW) / 2.0f))) +
      1;

  std::vector<float> linY(static_cast<size_t>(scaledW * scaledH));
  for (int y = 0; y < scaledH; ++y) {
    for (int x = 0; x < scaledW; ++x) {
      int p = y * scaledW + x;
      int i4 = p * 4;
      uint8_t r = scaledBuf[static_cast<size_t>(i4)];
      uint8_t g = scaledBuf[static_cast<size_t>(i4 + 1)];
      uint8_t b = scaledBuf[static_cast<size_t>(i4 + 2)];
      linY[static_cast<size_t>(p)] =
          255.0f *
          (0.2126f * srgb2lin(r) + 0.7152f * srgb2lin(g) +
           0.0722f * srgb2lin(b));
    }
  }

  std::unordered_map<uint32_t, int> hist;
  hist.reserve(static_cast<size_t>(scaledW * scaledH / 4));
  for (int p = 0; p < scaledW * scaledH; ++p) {
    int i4 = p * 4;
    if (scaledBuf[static_cast<size_t>(i4 + 3)] == 0) continue;
    uint8_t r = scaledBuf[static_cast<size_t>(i4)];
    uint8_t g = scaledBuf[static_cast<size_t>(i4 + 1)];
    uint8_t b = scaledBuf[static_cast<size_t>(i4 + 2)];
    uint32_t key = rgbToKey(r, g, b);
    ++hist[key];
  }
  uint32_t bgKey = 0;
  int bgCnt = 0;
  for (const auto& kv : hist) {
    if (kv.second > bgCnt) {
      bgCnt = kv.second;
      bgKey = kv.first;
    }
  }
  uint8_t bgR = static_cast<uint8_t>((bgKey >> 16) & 0xFF);
  uint8_t bgG = static_cast<uint8_t>((bgKey >> 8) & 0xFF);
  uint8_t bgB = static_cast<uint8_t>(bgKey & 0xFF);
  float bgLum = 255.0f * (0.2126f * srgb2lin(bgR) +
                          0.7152f * srgb2lin(bgG) +
                          0.0722f * srgb2lin(bgB));

  std::vector<float> err;
  if (useDith) err.assign(static_cast<size_t>(scaledW * scaledH), 0.0f);

  std::vector<float> edges;
  if (useEdge) {
    edges.resize(static_cast<size_t>(scaledW * scaledH));
    for (int y = 0; y < scaledH; ++y) {
      for (int x = 0; x < scaledW; ++x) {
        edges[static_cast<size_t>(y * scaledW + x)] =
            sobelAt(linY, scaledW, scaledH, x, y);
      }
    }
  }

  art.width = outW;
  art.height = outH;
  art.cells.resize(static_cast<size_t>(outW * outH));

  for (int cy = 0; cy < outH; ++cy) {
    for (int cx = 0; cx < outW; ++cx) {
      uint32_t k1 = 0, k2 = 0, k3 = 0;
      int c1 = 0, c2 = 0, c3 = 0;
      auto vote = [&](uint32_t key) {
        if (key == k1) {
          ++c1;
          return;
        }
        if (key == k2) {
          ++c2;
          return;
        }
        if (key == k3) {
          ++c3;
          return;
        }
        if (c1 <= c2 && c1 <= c3) {
          k1 = key;
          c1 = 1;
          return;
        }
        if (c2 <= c1 && c2 <= c3) {
          k2 = key;
          c2 = 1;
          return;
        }
        k3 = key;
        c3 = 1;
      };

      int cellBgR = 0;
      int cellBgG = 0;
      int cellBgB = 0;
      int cellBgCnt = 0;
      int bitmask = 0;

      for (int dy = 0; dy < 4; ++dy) {
        for (int dx = 0; dx < 2; ++dx) {
          int px = std::min(scaledW - 1, cx * 2 + dx);
          int py = std::min(scaledH - 1, cy * 4 + dy);
          int p = py * scaledW + px;
          int i4 = p * 4;
          uint8_t r = scaledBuf[static_cast<size_t>(i4)];
          uint8_t g = scaledBuf[static_cast<size_t>(i4 + 1)];
          uint8_t b = scaledBuf[static_cast<size_t>(i4 + 2)];

          float yLin = linY[static_cast<size_t>(p)];
          bool nearBg = colorDistSq(r, g, b, bgR, bgG, bgB) < bgDist;
          bool ditherThisPixel = useDith && !nearBg;
          if (ditherThisPixel && !err.empty()) {
            yLin = clampFloat(yLin + err[static_cast<size_t>(p)], 0.0f, 255.0f);
          }

          float deltaThr = delta;
          if (useEdge && !edges.empty()) {
            deltaThr = std::max(
                10.0f, delta - 0.2f * edges[static_cast<size_t>(p)]);
          }

          float lumDiff = std::fabs(yLin - bgLum);
          bool dotSet = !nearBg && lumDiff >= deltaThr;

          if (dotSet) {
            bitmask |= 1 << brailleMap[dx][dy];
            vote(rgbToKey(r, g, b));
          }

          if (nearBg) {
            cellBgR += r;
            cellBgG += g;
            cellBgB += b;
            ++cellBgCnt;
          }

          if (ditherThisPixel && !err.empty()) {
            float target = dotSet ? 0.0f : 255.0f;
            distributeError(err, yLin - target, p, scaledW, scaledH);
          }
        }
      }

      uint32_t domKey = 0x808080;
      int domCnt = 0;
      if (c1 > domCnt) {
        domCnt = c1;
        domKey = k1;
      }
      if (c2 > domCnt) {
        domCnt = c2;
        domKey = k2;
      }
      if (c3 > domCnt) {
        domCnt = c3;
        domKey = k3;
      }

      AsciiArt::AsciiCell cell{};
      cell.ch = static_cast<wchar_t>(kBrailleBase + bitmask);
      cell.fg = Color{static_cast<uint8_t>((domKey >> 16) & 0xFF),
                      static_cast<uint8_t>((domKey >> 8) & 0xFF),
                      static_cast<uint8_t>(domKey & 0xFF)};
      if (cellBgCnt > 0) {
        cell.hasBg = true;
        cell.bg = Color{
            clampByte(static_cast<float>(cellBgR) / cellBgCnt),
            clampByte(static_cast<float>(cellBgG) / cellBgCnt),
            clampByte(static_cast<float>(cellBgB) / cellBgCnt)};
      }
      art.cells[static_cast<size_t>(cy * outW + cx)] = cell;
    }
  }

  return art;
}
} // namespace

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

  int maxArtWidth = std::max(8, maxWidth);
  out = generateBrailleArt(rgba, imgW, imgH, maxArtWidth);
  if (maxHeight > 0 && out.height > maxHeight) {
    out.height = maxHeight;
    out.cells.resize(static_cast<size_t>(out.width * out.height));
  }
  return true;
}

bool renderAsciiArtFromRgba(const uint8_t* rgba,
                            int width,
                            int height,
                            int maxWidth,
                            int maxHeight,
                            AsciiArt& out) {
  out = AsciiArt{};
  if (!rgba || width <= 0 || height <= 0) return false;
  std::vector<uint8_t> buffer(static_cast<size_t>(width * height * 4));
  std::memcpy(buffer.data(), rgba, buffer.size());
  int maxArtWidth = std::max(8, maxWidth);
  out = generateBrailleArt(buffer, width, height, maxArtWidth);
  if (maxHeight > 0 && out.height > maxHeight) {
    out.height = maxHeight;
    out.cells.resize(static_cast<size_t>(out.width * out.height));
  }
  return true;
}
