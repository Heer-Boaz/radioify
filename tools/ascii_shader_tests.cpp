#include "asciiart.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <combaseapi.h>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <wincodec.h>
#include <windows.h>

namespace {

constexpr uint32_t kBrailleBase = 0x2800;

struct HarnessConfig {
  int width = 960;
  int height = 540;
  int maxColumns = 156;
  int maxRows = 54;
  std::string fixture = "all";
  std::string variant = "all";
  std::filesystem::path outputDir = "ascii_shader_tests_out";
};

struct RgbaImage {
  int width = 0;
  int height = 0;
  std::vector<uint8_t> pixels;
};

struct Rgb {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
};

struct Variant {
  const char* name;
  uint32_t stageMask;
};

[[noreturn]] void fail(const std::string& message) {
  throw std::runtime_error(message);
}

int parseIntArg(char** argv, int& index, int argc, const char* name) {
  if (index + 1 >= argc) fail(std::string("missing value for ") + name);
  ++index;
  char* end = nullptr;
  long value = std::strtol(argv[index], &end, 10);
  if (!end || *end != '\0') fail(std::string("invalid integer for ") + name);
  return static_cast<int>(value);
}

void printUsage() {
  std::cout
      << "ascii_shader_tests [options]\n"
      << "  --fixture <all|circle-checker|phone-edge|thin-lines>\n"
      << "  --variant <all|current|ink-only|no-dither|no-edge-detect|"
         "no-bg-swaps|no-minority-bg-swap|no-bright-bg-swap|"
         "no-edge-ink-preserve|no-edge-bg-tone|no-signal-dampen|no-detail-boost|"
         "no-temporal|no-bg-clamp|no-bg-luma-floor|no-bg-polish>\n"
      << "  --out-dir <path>\n"
      << "  --width <pixels>\n"
      << "  --height <pixels>\n"
      << "  --cols <terminal columns>\n"
      << "  --rows <terminal rows>\n";
}

HarnessConfig parseArgs(int argc, char** argv) {
  HarnessConfig config;
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "--fixture") {
      if (i + 1 >= argc) fail("missing value for --fixture");
      config.fixture = argv[++i];
    } else if (arg == "--variant") {
      if (i + 1 >= argc) fail("missing value for --variant");
      config.variant = argv[++i];
    } else if (arg == "--out-dir") {
      if (i + 1 >= argc) fail("missing value for --out-dir");
      config.outputDir = argv[++i];
    } else if (arg == "--width") {
      config.width = parseIntArg(argv, i, argc, "--width");
    } else if (arg == "--height") {
      config.height = parseIntArg(argv, i, argc, "--height");
    } else if (arg == "--cols") {
      config.maxColumns = parseIntArg(argv, i, argc, "--cols");
    } else if (arg == "--rows") {
      config.maxRows = parseIntArg(argv, i, argc, "--rows");
    } else if (arg == "--help" || arg == "-h") {
      printUsage();
      std::exit(0);
    } else {
      fail(std::string("unknown argument: ") + std::string(arg));
    }
  }
  if (config.width <= 0 || config.height <= 0) {
    fail("source dimensions must be positive");
  }
  if (config.maxColumns <= 0 || config.maxRows <= 0) {
    fail("ASCII dimensions must be positive");
  }
  return config;
}

uint32_t hash2(int x, int y, uint32_t seed) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u +
               static_cast<uint32_t>(y) * 668265263u + seed * 1442695041u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

uint8_t clampByte(int value) {
  return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

Rgb addNoise(Rgb c, int x, int y, uint32_t seed, int strength) {
  int n = static_cast<int>(hash2(x, y, seed) & 31u) - 15;
  return Rgb{clampByte(static_cast<int>(c.r) + n * strength / 8),
             clampByte(static_cast<int>(c.g) + n * strength / 8),
             clampByte(static_cast<int>(c.b) + n * strength / 8)};
}

Rgb mix(Rgb a, Rgb b, float t) {
  float u = std::clamp(t, 0.0f, 1.0f);
  return Rgb{clampByte(static_cast<int>(std::lround(a.r + (b.r - a.r) * u))),
             clampByte(static_cast<int>(std::lround(a.g + (b.g - a.g) * u))),
             clampByte(static_cast<int>(std::lround(a.b + (b.b - a.b) * u)))};
}

void setPixel(RgbaImage& image, int x, int y, Rgb c) {
  size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
  image.pixels[offset + 0] = c.r;
  image.pixels[offset + 1] = c.g;
  image.pixels[offset + 2] = c.b;
  image.pixels[offset + 3] = 255;
}

void blendPixel(RgbaImage& image, int x, int y, Rgb c, float alpha) {
  size_t offset = (static_cast<size_t>(y) * image.width + x) * 4;
  Rgb base{image.pixels[offset + 0], image.pixels[offset + 1],
           image.pixels[offset + 2]};
  Rgb out = mix(base, c, alpha);
  image.pixels[offset + 0] = out.r;
  image.pixels[offset + 1] = out.g;
  image.pixels[offset + 2] = out.b;
  image.pixels[offset + 3] = 255;
}

RgbaImage makeImage(int width, int height) {
  RgbaImage image;
  image.width = width;
  image.height = height;
  image.pixels.assign(static_cast<size_t>(width) * height * 4, 255);
  return image;
}

void fillNoisyChecker(RgbaImage& image, Rgb a, Rgb b, int cellW, int cellH,
                      uint32_t seed) {
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      bool checker = ((x / cellW) + (y / cellH)) & 1;
      Rgb base = checker ? a : b;
      int wave = static_cast<int>(12.0 * std::sin(x * 0.023 + y * 0.017));
      base = Rgb{clampByte(static_cast<int>(base.r) + wave),
                 clampByte(static_cast<int>(base.g) + wave),
                 clampByte(static_cast<int>(base.b) + wave)};
      setPixel(image, x, y, addNoise(base, x, y, seed, 5));
    }
  }
}

RgbaImage makeCircleCheckerFixture(int width, int height) {
  RgbaImage image = makeImage(width, height);
  fillNoisyChecker(image, Rgb{22, 30, 36}, Rgb{48, 55, 60}, 18, 14, 19);

  float cx = width * 0.52f;
  float cy = height * 0.50f;
  float radius = std::min(width, height) * 0.24f;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float dx = x + 0.5f - cx;
      float dy = y + 0.5f - cy;
      float d = std::sqrt(dx * dx + dy * dy);
      float fill = std::clamp((radius - d + 1.5f) / 3.0f, 0.0f, 1.0f);
      if (fill > 0.0f) {
        blendPixel(image, x, y, Rgb{64, 34, 78}, fill);
      }
      float rim = std::clamp((3.2f - std::abs(d - radius)) / 3.2f, 0.0f, 1.0f);
      if (rim > 0.0f) {
        blendPixel(image, x, y, Rgb{120, 204, 214}, rim);
      }
    }
  }
  return image;
}

float roundedBoxSdf(float x, float y, float halfW, float halfH, float radius) {
  float qx = std::fabs(x) - halfW + radius;
  float qy = std::fabs(y) - halfH + radius;
  float ox = std::max(qx, 0.0f);
  float oy = std::max(qy, 0.0f);
  float outside = std::sqrt(ox * ox + oy * oy) - radius;
  float inside = std::min(std::max(qx, qy), 0.0f);
  return outside + inside;
}

RgbaImage makePhoneEdgeFixture(int width, int height) {
  RgbaImage image = makeImage(width, height);
  fillNoisyChecker(image, Rgb{16, 24, 31}, Rgb{38, 48, 56}, 22, 18, 37);

  float angle = -0.32f;
  float ca = std::cos(angle);
  float sa = std::sin(angle);
  float cx = width * 0.55f;
  float cy = height * 0.50f;
  float halfW = width * 0.31f;
  float halfH = height * 0.22f;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float dx = x + 0.5f - cx;
      float dy = y + 0.5f - cy;
      float px = ca * dx - sa * dy;
      float py = sa * dx + ca * dy;
      float body = roundedBoxSdf(px, py, halfW, halfH, 24.0f);
      float bodyAlpha = std::clamp((-body + 1.5f) / 3.0f, 0.0f, 1.0f);
      if (bodyAlpha > 0.0f) {
        blendPixel(image, x, y, Rgb{10, 13, 16}, bodyAlpha);
      }
      float rim = std::clamp((5.0f - std::fabs(body)) / 5.0f, 0.0f, 1.0f);
      if (rim > 0.0f) {
        blendPixel(image, x, y, Rgb{95, 160, 170}, rim);
      }

      float screen = roundedBoxSdf(px, py, halfW * 0.72f, halfH * 0.68f, 8.0f);
      float screenAlpha = std::clamp((-screen + 1.0f) / 2.0f, 0.0f, 1.0f);
      if (screenAlpha > 0.0f) {
        blendPixel(image, x, y, Rgb{31, 19, 42}, screenAlpha);
      }
      float screenLine =
          std::clamp((2.0f - std::fabs(screen)) / 2.0f, 0.0f, 1.0f);
      if (screenLine > 0.0f) {
        blendPixel(image, x, y, Rgb{64, 72, 90}, screenLine);
      }
    }
  }

  auto drawButton = [&](float bx, float by, float radius, Rgb color) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        float dx = x + 0.5f - bx;
        float dy = y + 0.5f - by;
        float d = std::sqrt(dx * dx + dy * dy);
        float alpha = std::clamp((radius - d + 1.5f) / 3.0f, 0.0f, 1.0f);
        if (alpha > 0.0f) blendPixel(image, x, y, color, alpha);
      }
    }
  };
  drawButton(width * 0.66f, height * 0.36f, height * 0.035f, Rgb{24, 235, 18});
  drawButton(width * 0.72f, height * 0.53f, height * 0.032f, Rgb{238, 30, 54});
  drawButton(width * 0.50f, height * 0.58f, height * 0.020f, Rgb{190, 150, 218});
  return image;
}

RgbaImage makeThinLinesFixture(int width, int height) {
  RgbaImage image = makeImage(width, height);
  fillNoisyChecker(image, Rgb{54, 39, 35}, Rgb{92, 68, 52}, 16, 16, 53);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      float lineA = std::fabs(std::fmod(x * 0.7f + y * 1.3f, 44.0f) - 22.0f);
      float alphaA = std::clamp((2.0f - lineA) / 2.0f, 0.0f, 1.0f);
      if (alphaA > 0.0f) blendPixel(image, x, y, Rgb{236, 202, 126}, alphaA);
      float lineB = std::fabs(std::fmod(x * 1.1f - y * 0.9f, 57.0f) - 28.5f);
      float alphaB = std::clamp((1.5f - lineB) / 1.5f, 0.0f, 1.0f);
      if (alphaB > 0.0f) blendPixel(image, x, y, Rgb{16, 20, 24}, alphaB);
    }
  }
  return image;
}

RgbaImage makeFixture(std::string_view fixture, int width, int height) {
  if (fixture == "circle-checker") return makeCircleCheckerFixture(width, height);
  if (fixture == "phone-edge") return makePhoneEdgeFixture(width, height);
  if (fixture == "thin-lines") return makeThinLinesFixture(width, height);
  fail(std::string("unknown fixture: ") + std::string(fixture));
}

bool wants(std::string_view selected, std::string_view name) {
  return selected == "all" || selected == name;
}

std::vector<std::string_view> selectedFixtures(const HarnessConfig& config) {
  static constexpr std::array<std::string_view, 3> kFixtures{
      "circle-checker", "phone-edge", "thin-lines"};
  std::vector<std::string_view> out;
  for (std::string_view fixture : kFixtures) {
    if (wants(config.fixture, fixture)) out.push_back(fixture);
  }
  if (out.empty()) fail("no fixture matched selection");
  return out;
}

std::array<Variant, 16> variants() {
  using namespace ascii_debug;
  uint32_t all = kAllStages;
  return {{
      {"current", all},
      {"ink-only", all & ~kStageCellBackground},
      {"no-dither", all & ~kStageDither},
      {"no-edge-detect", all & ~kStageEdgeDetect},
      {"no-bg-swaps", all & ~kStageMinorityBgSwap & ~kStageBrightBgSwap},
      {"no-minority-bg-swap", all & ~kStageMinorityBgSwap},
      {"no-bright-bg-swap", all & ~kStageBrightBgSwap},
      {"no-edge-ink-preserve", all & ~kStageEdgeInkPreserve},
      {"no-edge-bg-tone", all & ~kStageEdgeBgTone},
      {"no-signal-dampen", all & ~kStageSignalDampen},
      {"no-detail-boost", all & ~kStageDetailBoost},
      {"no-temporal", all & ~kStageForegroundTemporal &
                          ~kStageBackgroundTemporal},
      {"no-bg-clamp", all & ~kStageEdgeBgClamp},
      {"no-bg-luma-floor", all & ~kStageBgLumaFloor},
      {"no-bg-polish", all & ~kStageBgLumaFloor &
                           ~kStageBackgroundTemporal &
                           ~kStageFullMaskBgContrast & ~kStageEdgeBgTone &
                           ~kStageEdgeInkPreserve & ~kStageEdgeBgClamp},
      {"structure-no-bg", all & ~kStageCellBackground & ~kStageDither},
  }};
}

std::vector<Variant> selectedVariants(const HarnessConfig& config) {
  std::vector<Variant> out;
  for (const Variant& variant : variants()) {
    if (wants(config.variant, variant.name)) out.push_back(variant);
  }
  if (out.empty()) fail("no variant matched selection");
  return out;
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
  ComScope() {
    hr_ = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr_) && hr_ != RPC_E_CHANGED_MODE) {
      fail("failed to initialize COM for PNG encoding");
    }
  }

  ~ComScope() {
    if (SUCCEEDED(hr_)) {
      CoUninitialize();
    }
  }

 private:
  HRESULT hr_ = E_FAIL;
};

void requireHr(HRESULT hr, const char* message) {
  if (FAILED(hr)) fail(message);
}

void writePngRgb(const std::filesystem::path& path, int width, int height,
                 const std::vector<uint8_t>& rgb) {
  if (width <= 0 || height <= 0) fail("invalid PNG dimensions");
  if (rgb.size() != static_cast<size_t>(width) * height * 3) {
    fail("invalid PNG buffer size");
  }

  std::vector<uint8_t> bgr(rgb.size());
  for (size_t i = 0; i < rgb.size(); i += 3) {
    bgr[i + 0] = rgb[i + 2];
    bgr[i + 1] = rgb[i + 1];
    bgr[i + 2] = rgb[i + 0];
  }

  ComScope com;
  IWICImagingFactory* factory = nullptr;
  requireHr(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                             CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "failed to create WIC factory");

  IWICStream* stream = nullptr;
  IWICBitmapEncoder* encoder = nullptr;
  IWICBitmapFrameEncode* frame = nullptr;
  IPropertyBag2* properties = nullptr;
  HRESULT hr = S_OK;
  auto cleanup = [&]() {
    safeRelease(properties);
    safeRelease(frame);
    safeRelease(encoder);
    safeRelease(stream);
    safeRelease(factory);
  };

  hr = factory->CreateStream(&stream);
  if (SUCCEEDED(hr)) {
    hr = stream->InitializeFromFilename(path.wstring().c_str(), GENERIC_WRITE);
  }
  if (SUCCEEDED(hr)) {
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->CreateNewFrame(&frame, &properties);
  }
  if (SUCCEEDED(hr)) {
    hr = frame->Initialize(properties);
  }
  if (SUCCEEDED(hr)) {
    hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
  if (SUCCEEDED(hr)) {
    hr = frame->SetPixelFormat(&format);
  }
  if (SUCCEEDED(hr) && !IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) {
    hr = E_FAIL;
  }
  if (SUCCEEDED(hr)) {
    const UINT stride = static_cast<UINT>(width * 3);
    const UINT size = static_cast<UINT>(bgr.size());
    hr = frame->WritePixels(static_cast<UINT>(height), stride, size,
                            bgr.data());
  }
  if (SUCCEEDED(hr)) {
    hr = frame->Commit();
  }
  if (SUCCEEDED(hr)) {
    hr = encoder->Commit();
  }
  cleanup();
  requireHr(hr, "failed to write PNG");
}

void writeSourcePng(const std::filesystem::path& path, const RgbaImage& image) {
  std::vector<uint8_t> rgb;
  rgb.reserve(static_cast<size_t>(image.width) * image.height * 3);
  for (size_t i = 0; i < image.pixels.size(); i += 4) {
    rgb.push_back(image.pixels[i + 0]);
    rgb.push_back(image.pixels[i + 1]);
    rgb.push_back(image.pixels[i + 2]);
  }
  writePngRgb(path, image.width, image.height, rgb);
}

void appendUtf8(std::string& out, uint32_t cp) {
  if (cp <= 0x7Fu) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFu) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFu) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

uint32_t brailleMaskForCell(const AsciiArt::AsciiCell& cell) {
  uint32_t cp = static_cast<uint32_t>(cell.ch);
  if (cp < kBrailleBase || cp > kBrailleBase + 0xFFu) return 0;
  return cp - kBrailleBase;
}

void writePlainText(const std::filesystem::path& path, const AsciiArt& art) {
  std::ofstream file(path, std::ios::binary);
  if (!file) fail("failed to open " + path.string());
  for (int y = 0; y < art.height; ++y) {
    std::string line;
    for (int x = 0; x < art.width; ++x) {
      const auto& cell = art.cells[static_cast<size_t>(y) * art.width + x];
      appendUtf8(line, static_cast<uint32_t>(cell.ch));
    }
    file << line << '\n';
  }
}

void writeAnsi(const std::filesystem::path& path, const AsciiArt& art) {
  std::ofstream file(path, std::ios::binary);
  if (!file) fail("failed to open " + path.string());
  for (int y = 0; y < art.height; ++y) {
    for (int x = 0; x < art.width; ++x) {
      const auto& cell = art.cells[static_cast<size_t>(y) * art.width + x];
      file << "\x1b[38;2;" << static_cast<int>(cell.fg.r) << ';'
           << static_cast<int>(cell.fg.g) << ';'
           << static_cast<int>(cell.fg.b) << 'm';
      if (cell.hasBg) {
        file << "\x1b[48;2;" << static_cast<int>(cell.bg.r) << ';'
             << static_cast<int>(cell.bg.g) << ';'
             << static_cast<int>(cell.bg.b) << 'm';
      } else {
        file << "\x1b[49m";
      }
      std::string glyph;
      appendUtf8(glyph, static_cast<uint32_t>(cell.ch));
      file << glyph;
    }
    file << "\x1b[0m\n";
  }
}

void fillRgbRect(std::vector<uint8_t>& rgb, int width, int height, int x0,
                 int y0, int x1, int y1, Rgb color) {
  int left = std::clamp(x0, 0, width);
  int top = std::clamp(y0, 0, height);
  int right = std::clamp(x1, 0, width);
  int bottom = std::clamp(y1, 0, height);
  for (int y = top; y < bottom; ++y) {
    for (int x = left; x < right; ++x) {
      size_t offset = (static_cast<size_t>(y) * width + x) * 3;
      rgb[offset + 0] = color.r;
      rgb[offset + 1] = color.g;
      rgb[offset + 2] = color.b;
    }
  }
}

void writeAsciiRasterPng(const std::filesystem::path& path,
                         const AsciiArt& art) {
  constexpr int cellW = 8;
  constexpr int cellH = 12;
  int width = art.width * cellW;
  int height = art.height * cellH;
  std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3, 0);
  constexpr int bitToX[8] = {0, 0, 0, 1, 1, 1, 0, 1};
  constexpr int bitToY[8] = {0, 1, 2, 0, 1, 2, 3, 3};

  for (int cy = 0; cy < art.height; ++cy) {
    for (int cx = 0; cx < art.width; ++cx) {
      const auto& cell = art.cells[static_cast<size_t>(cy) * art.width + cx];
      Rgb bg = cell.hasBg ? Rgb{cell.bg.r, cell.bg.g, cell.bg.b}
                          : Rgb{0, 0, 0};
      int px = cx * cellW;
      int py = cy * cellH;
      fillRgbRect(rgb, width, height, px, py, px + cellW, py + cellH, bg);

      Rgb fg{cell.fg.r, cell.fg.g, cell.fg.b};
      uint32_t mask = brailleMaskForCell(cell);
      for (int bit = 0; bit < 8; ++bit) {
        if ((mask & (1u << bit)) == 0) continue;
        int dotX = px + bitToX[bit] * 4 + 1;
        int dotY = py + bitToY[bit] * 3 + 1;
        fillRgbRect(rgb, width, height, dotX, dotY, dotX + 3, dotY + 2, fg);
      }
    }
  }
  writePngRgb(path, width, height, rgb);
}

double percent(uint64_t value, uint64_t total) {
  if (total == 0) return 0.0;
  return (static_cast<double>(value) * 100.0) / static_cast<double>(total);
}

double averageDots(const ascii_debug::RenderStats& stats) {
  uint64_t dots = 0;
  uint64_t cells = 0;
  for (int i = 0; i <= 8; ++i) {
    dots += static_cast<uint64_t>(i) * stats.dotCountHistogram[i];
    cells += stats.dotCountHistogram[i];
  }
  if (cells == 0) return 0.0;
  return static_cast<double>(dots) / static_cast<double>(cells);
}

void writeCsvHeader(std::ostream& out) {
  out << "fixture,variant,width,height,cells,bg_cells,bg_pct,avg_dots,"
         "dither_cells,edge_cells,minority_bg_swaps,bright_bg_swaps,"
         "edge_ink_preserved,edge_bg_toned,signal_dampened,detail_boosted,"
         "ink_lifted,bg_lifted,"
         "fg_temporal,bg_temporal,fullmask_bg_contrast,bg_clamped\n";
}

void writeCsvRow(std::ostream& out, std::string_view fixture,
                 std::string_view variant, const AsciiArt& art,
                 const ascii_debug::RenderStats& stats) {
  out << fixture << ',' << variant << ',' << art.width << ',' << art.height
      << ',' << stats.cellCount << ',' << stats.bgCellCount << ','
      << std::fixed << std::setprecision(2)
      << percent(stats.bgCellCount, stats.cellCount) << ','
      << std::setprecision(3) << averageDots(stats) << std::setprecision(2)
      << ',' << stats.ditherCellCount << ',' << stats.edgeCellCount << ','
      << stats.minorityBgSwapCount << ',' << stats.brightBgSwapCount << ','
      << stats.edgeInkPreserveCount << ',' << stats.edgeBgToneCount << ','
      << stats.signalDampenCount << ','
      << stats.detailBoostCount << ',' << stats.inkLumaFloorCount << ','
      << stats.bgLumaFloorCount << ',' << stats.fgTemporalBlendCount << ','
      << stats.bgTemporalBlendCount << ','
      << stats.fullMaskBgContrastCount << ',' << stats.edgeBgClampCount
      << '\n';
}

void renderVariant(const HarnessConfig& config, std::string_view fixtureName,
                   const Variant& variant, const RgbaImage& image,
                   std::ostream& csv) {
  AsciiArt art;
  ascii_debug::RenderOptions options;
  options.stageMask = variant.stageMask;
  options.resetHistory = true;
  ascii_debug::RenderStats stats;
  if (!renderAsciiArtFromRgbaDebug(image.pixels.data(), image.width,
                                   image.height, config.maxColumns,
                                   config.maxRows, art, options, &stats,
                                   true)) {
    fail("ASCII render failed");
  }

  std::filesystem::path fixtureDir = config.outputDir / std::string(fixtureName);
  std::filesystem::create_directories(fixtureDir);
  std::string base = std::string(variant.name);
  writeAnsi(fixtureDir / (base + ".ans"), art);
  writePlainText(fixtureDir / (base + ".txt"), art);
  writeAsciiRasterPng(fixtureDir / (base + ".png"), art);
  writeCsvRow(csv, fixtureName, variant.name, art, stats);

  std::cout << std::setw(16) << fixtureName << "  " << std::setw(22)
            << variant.name << "  bg="
            << std::fixed << std::setprecision(1)
            << percent(stats.bgCellCount, stats.cellCount) << "%  dots="
            << std::setprecision(2) << averageDots(stats)
            << "  swaps=" << (stats.minorityBgSwapCount +
                               stats.brightBgSwapCount)
            << "  preserve=" << stats.edgeInkPreserveCount
            << "  clamp=" << stats.edgeBgClampCount << '\n';
}

void runHarness(const HarnessConfig& config) {
  std::filesystem::create_directories(config.outputDir);
  std::ofstream csv(config.outputDir / "summary.csv", std::ios::binary);
  if (!csv) fail("failed to open summary.csv");
  writeCsvHeader(csv);

  std::vector<Variant> variantList = selectedVariants(config);
  for (std::string_view fixtureName : selectedFixtures(config)) {
    RgbaImage image = makeFixture(fixtureName, config.width, config.height);
    std::filesystem::path fixtureDir =
        config.outputDir / std::string(fixtureName);
    std::filesystem::create_directories(fixtureDir);
    writeSourcePng(fixtureDir / "source.png", image);
    for (const Variant& variant : variantList) {
      renderVariant(config, fixtureName, variant, image, csv);
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    runHarness(parseArgs(argc, argv));
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "ascii_shader_tests: " << e.what() << '\n';
    return 1;
  }
}
