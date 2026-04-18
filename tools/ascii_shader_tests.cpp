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

struct TuningAssignment {
  std::string name;
  int value = 0;
};

struct TuningAxis {
  std::string name;
  std::vector<int> values;
};

struct HarnessConfig {
  int width = 960;
  int height = 540;
  int maxColumns = 156;
  int maxRows = 54;
  std::string fixture = "all";
  std::string variant = "all";
  std::filesystem::path outputDir = "ascii_shader_tests_out";
  std::vector<TuningAssignment> fixedTunings;
  std::vector<TuningAxis> tuningAxes;
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
  std::string name;
  uint32_t stageMask;
  ascii_debug::RenderOptions::TuningOverrides tuning;
};

struct RenderedVariant {
  AsciiArt art;
  ascii_debug::RenderStats stats;
};

struct OutputDelta {
  uint64_t cellCount = 0;
  uint64_t changedCells = 0;
  uint64_t glyphChanged = 0;
  uint64_t maskChanged = 0;
  uint64_t dotCountChanged = 0;
  uint64_t hasBgChanged = 0;
  uint64_t fgChanged = 0;
  uint64_t bgChanged = 0;
  uint64_t fgDeltaGt2 = 0;
  uint64_t bgDeltaGt2 = 0;
  uint64_t fgDeltaGt8 = 0;
  uint64_t bgDeltaGt8 = 0;
  uint64_t fgDeltaSum = 0;
  uint64_t bgDeltaSum = 0;
  double bgLumaDeltaSum = 0.0;
  int fgDeltaMax = 0;
  int bgDeltaMax = 0;
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

int parseIntValue(std::string_view text, std::string_view context) {
  std::string valueText(text);
  char* end = nullptr;
  long value = std::strtol(valueText.c_str(), &end, 10);
  if (!end || *end != '\0' || value < 0 || value > 4096) {
    fail("invalid tuning value for " + std::string(context));
  }
  return static_cast<int>(value);
}

std::vector<std::string_view> splitCsv(std::string_view text) {
  std::vector<std::string_view> parts;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t comma = text.find(',', pos);
    if (comma == std::string_view::npos) comma = text.size();
    std::string_view part = text.substr(pos, comma - pos);
    if (part.empty()) fail("empty comma-separated value");
    parts.push_back(part);
    if (comma == text.size()) break;
    pos = comma + 1;
  }
  return parts;
}

TuningAssignment parseTuningAssignment(std::string_view text) {
  size_t eq = text.find('=');
  if (eq == std::string_view::npos || eq == 0 || eq + 1 >= text.size()) {
    fail("expected tuning assignment name=value");
  }
  TuningAssignment assignment;
  assignment.name = std::string(text.substr(0, eq));
  assignment.value = parseIntValue(text.substr(eq + 1), assignment.name);
  return assignment;
}

void parseTuningAssignments(std::string_view text,
                            std::vector<TuningAssignment>& out) {
  for (std::string_view part : splitCsv(text)) {
    out.push_back(parseTuningAssignment(part));
  }
}

TuningAxis parseTuningAxis(std::string_view text) {
  size_t eq = text.find('=');
  if (eq == std::string_view::npos || eq == 0 || eq + 1 >= text.size()) {
    fail("expected sweep axis name=value,value,...");
  }
  TuningAxis axis;
  axis.name = std::string(text.substr(0, eq));
  for (std::string_view value : splitCsv(text.substr(eq + 1))) {
    axis.values.push_back(parseIntValue(value, axis.name));
  }
  return axis;
}

void printUsage() {
  std::cout
      << "ascii_shader_tests [options]\n"
      << "  --fixture <all|circle-checker|phone-edge|thin-lines|"
         "fullmask-contrast>\n"
      << "  --variant <all|current|ink-only|no-dither|no-edge-detect|"
         "no-signal-dampen|no-detail-boost|no-edge-mask-fit|"
         "no-ink-coverage|"
         "no-temporal|no-bg-polish|structure-no-bg>\n"
      << "  --out-dir <path>\n"
      << "  --width <pixels>\n"
      << "  --height <pixels>\n"
      << "  --cols <terminal columns>\n"
      << "  --rows <terminal rows>\n"
      << "  --tune <name=value[,name=value...]>\n"
      << "  --sweep <name=value,value,...>\n"
      << "  tuning names: inkcov,covmax,covsignal,covminluma,"
         "fitgain,fitrange,fitsignal,edgefloor,ditheredge,signalfloor,"
         "inkmin,inkmaxscale,saturation,lumaw,bgpenalty,inkprio\n";
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
    } else if (arg == "--tune") {
      if (i + 1 >= argc) fail("missing value for --tune");
      parseTuningAssignments(argv[++i], config.fixedTunings);
    } else if (arg == "--sweep") {
      if (i + 1 >= argc) fail("missing value for --sweep");
      config.tuningAxes.push_back(parseTuningAxis(argv[++i]));
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

RgbaImage makeFullMaskContrastFixture(int width, int height) {
  RgbaImage image = makeImage(width, height);
  fillNoisyChecker(image, Rgb{24, 28, 32}, Rgb{34, 39, 44}, 28, 22, 71);

  auto fillRect = [&](float left, float top, float right, float bottom,
                      Rgb color, uint32_t seed) {
    int x0 = std::clamp(static_cast<int>(std::lround(left * width)), 0, width);
    int y0 = std::clamp(static_cast<int>(std::lround(top * height)), 0, height);
    int x1 = std::clamp(static_cast<int>(std::lround(right * width)), 0, width);
    int y1 =
        std::clamp(static_cast<int>(std::lround(bottom * height)), 0, height);
    for (int y = y0; y < y1; ++y) {
      for (int x = x0; x < x1; ++x) {
        setPixel(image, x, y, addNoise(color, x, y, seed, 1));
      }
    }
  };

  fillRect(0.10f, 0.14f, 0.38f, 0.36f, Rgb{214, 224, 232}, 83);
  fillRect(0.45f, 0.18f, 0.88f, 0.40f, Rgb{184, 198, 212}, 89);
  fillRect(0.16f, 0.56f, 0.68f, 0.80f, Rgb{230, 214, 170}, 97);
  fillRect(0.72f, 0.58f, 0.92f, 0.82f, Rgb{206, 230, 202}, 101);

  return image;
}

RgbaImage makeFixture(std::string_view fixture, int width, int height) {
  if (fixture == "circle-checker") return makeCircleCheckerFixture(width, height);
  if (fixture == "phone-edge") return makePhoneEdgeFixture(width, height);
  if (fixture == "thin-lines") return makeThinLinesFixture(width, height);
  if (fixture == "fullmask-contrast") {
    return makeFullMaskContrastFixture(width, height);
  }
  fail(std::string("unknown fixture: ") + std::string(fixture));
}

bool wants(std::string_view selected, std::string_view name) {
  return selected == "all" || selected == name;
}

bool wants(std::string_view selected, const std::string& name) {
  return wants(selected, std::string_view(name.data(), name.size()));
}

std::vector<std::string_view> selectedFixtures(const HarnessConfig& config) {
  static constexpr std::array<std::string_view, 4> kFixtures{
      "circle-checker", "phone-edge", "thin-lines", "fullmask-contrast"};
  std::vector<std::string_view> out;
  for (std::string_view fixture : kFixtures) {
    if (wants(config.fixture, fixture)) out.push_back(fixture);
  }
  if (out.empty()) fail("no fixture matched selection");
  return out;
}

std::string canonicalTuningName(std::string_view name) {
  if (name == "inkcov" || name == "inkVisibleDotCoverage") return "inkcov";
  if (name == "covmax" || name == "inkCoverageMaxScale") return "covmax";
  if (name == "covsignal" || name == "inkCoverageMinSignal") {
    return "covsignal";
  }
  if (name == "covminluma" || name == "inkCoverageMinLuma") {
    return "covminluma";
  }
  if (name == "fitgain" || name == "edgeMaskFitMinGain") return "fitgain";
  if (name == "fitrange" || name == "edgeMaskFitMinRange") {
    return "fitrange";
  }
  if (name == "fitsignal" || name == "edgeMaskFitMinSignal") {
    return "fitsignal";
  }
  if (name == "lumaw" || name == "perceptualLumaErrorWeight") {
    return "lumaw";
  }
  if (name == "bgpenalty" || name == "perceptualBrightBgPenalty") {
    return "bgpenalty";
  }
  if (name == "inkprio" ||
      name == "perceptualPreferredBrightBgInkContrast") {
    return "inkprio";
  }
  if (name == "edgefloor" || name == "edgeThresholdFloor") {
    return "edgefloor";
  }
  if (name == "ditheredge" || name == "ditherMaxEdge") return "ditheredge";
  if (name == "signalfloor" || name == "signalStrengthFloor") {
    return "signalfloor";
  }
  if (name == "inkmin" || name == "inkMinLuma") return "inkmin";
  if (name == "inkmaxscale" || name == "inkMaxScale") {
    return "inkmaxscale";
  }
  if (name == "saturation" || name == "colorSaturation") {
    return "saturation";
  }
  fail("unknown tuning name: " + std::string(name));
}

void applyTuningAssignment(
    ascii_debug::RenderOptions::TuningOverrides& tuning,
    const TuningAssignment& assignment) {
  std::string name = canonicalTuningName(assignment.name);
  int value = assignment.value;
  if (name == "inkcov") {
    tuning.inkVisibleDotCoverage = value;
  } else if (name == "covmax") {
    tuning.inkCoverageMaxScale = value;
  } else if (name == "covsignal") {
    tuning.inkCoverageMinSignal = value;
  } else if (name == "covminluma") {
    tuning.inkCoverageMinLuma = value;
  } else if (name == "fitgain") {
    tuning.edgeMaskFitMinGain = value;
  } else if (name == "fitrange") {
    tuning.edgeMaskFitMinRange = value;
  } else if (name == "fitsignal") {
    tuning.edgeMaskFitMinSignal = value;
  } else if (name == "lumaw") {
    tuning.perceptualLumaErrorWeight = value;
  } else if (name == "bgpenalty") {
    tuning.perceptualBrightBgPenalty = value;
  } else if (name == "inkprio") {
    tuning.perceptualPreferredBrightBgInkContrast = value;
  } else if (name == "edgefloor") {
    tuning.edgeThresholdFloor = value;
  } else if (name == "ditheredge") {
    tuning.ditherMaxEdge = value;
  } else if (name == "signalfloor") {
    tuning.signalStrengthFloor = value;
  } else if (name == "inkmin") {
    tuning.inkMinLuma = value;
  } else if (name == "inkmaxscale") {
    tuning.inkMaxScale = value;
  } else if (name == "saturation") {
    tuning.colorSaturation = value;
  }
}

std::string tuningLabel(const std::vector<TuningAssignment>& assignments) {
  std::string label = "tune";
  for (const TuningAssignment& assignment : assignments) {
    label += '-';
    label += canonicalTuningName(assignment.name);
    label += std::to_string(assignment.value);
  }
  return label;
}

void buildTuningVariantsRecursive(
    const HarnessConfig& config, size_t axisIndex,
    std::vector<TuningAssignment>& assignments, std::vector<Variant>& out) {
  if (axisIndex == config.tuningAxes.size()) {
    if (assignments.empty()) return;
    Variant variant;
    variant.name = tuningLabel(assignments);
    variant.stageMask = ascii_debug::kAllStages;
    for (const TuningAssignment& assignment : assignments) {
      applyTuningAssignment(variant.tuning, assignment);
    }
    out.push_back(std::move(variant));
    return;
  }

  const TuningAxis& axis = config.tuningAxes[axisIndex];
  for (int value : axis.values) {
    assignments.push_back(TuningAssignment{axis.name, value});
    buildTuningVariantsRecursive(config, axisIndex + 1, assignments, out);
    assignments.pop_back();
  }
}

std::vector<Variant> tuningVariants(const HarnessConfig& config) {
  std::vector<Variant> out;
  std::vector<TuningAssignment> assignments = config.fixedTunings;
  buildTuningVariantsRecursive(config, 0, assignments, out);
  return out;
}

std::array<Variant, 11> variants() {
  using namespace ascii_debug;
  uint32_t all = kAllStages;
  return {{
      {"current", all, {}},
      {"ink-only", all & ~kStageCellBackground, {}},
      {"no-dither", all & ~kStageDither, {}},
      {"no-edge-detect", all & ~kStageEdgeDetect, {}},
      {"no-signal-dampen", all & ~kStageSignalDampen, {}},
      {"no-detail-boost", all & ~kStageDetailBoost, {}},
      {"no-edge-mask-fit", all & ~kStageEdgeMaskFit, {}},
      {"no-ink-coverage", all & ~kStageInkCoverageCompensation, {}},
      {"no-temporal", all & ~kStageForegroundTemporal &
                          ~kStageBackgroundTemporal, {}},
      {"no-bg-polish", all & ~kStageBackgroundTemporal &
                           ~kStageFullMaskBgContrast, {}},
      {"structure-no-bg", all & ~kStageCellBackground & ~kStageDither, {}},
  }};
}

std::vector<Variant> selectedVariants(const HarnessConfig& config) {
  std::vector<Variant> out;
  for (const Variant& variant : variants()) {
    if (wants(config.variant, variant.name)) out.push_back(variant);
  }
  for (Variant& variant : tuningVariants(config)) {
    if (config.variant == "all" || config.variant == "current" ||
        config.variant == "tune" || config.variant == "tuned" ||
        wants(config.variant, variant.name)) {
      out.push_back(std::move(variant));
    }
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

int dotCount(uint32_t mask) {
  int count = 0;
  while (mask != 0) {
    count += static_cast<int>(mask & 1u);
    mask >>= 1;
  }
  return count;
}

int colorDeltaMax(const Color& a, const Color& b) {
  return std::max({std::abs(static_cast<int>(a.r) - static_cast<int>(b.r)),
                   std::abs(static_cast<int>(a.g) - static_cast<int>(b.g)),
                   std::abs(static_cast<int>(a.b) - static_cast<int>(b.b))});
}

double colorLuma(const Color& c) {
  return (0.2126 * static_cast<double>(c.r)) +
         (0.7152 * static_cast<double>(c.g)) +
         (0.0722 * static_cast<double>(c.b));
}

OutputDelta compareArt(const AsciiArt& baseline, const AsciiArt& candidate) {
  if (baseline.width != candidate.width || baseline.height != candidate.height ||
      baseline.cells.size() != candidate.cells.size()) {
    fail("cannot compare ASCII art with different dimensions");
  }

  OutputDelta delta;
  delta.cellCount = static_cast<uint64_t>(baseline.cells.size());
  for (size_t i = 0; i < baseline.cells.size(); ++i) {
    const auto& a = baseline.cells[i];
    const auto& b = candidate.cells[i];
    const uint32_t maskA = brailleMaskForCell(a);
    const uint32_t maskB = brailleMaskForCell(b);
    const int fgDelta = colorDeltaMax(a.fg, b.fg);
    const int bgDelta = colorDeltaMax(a.bg, b.bg);
    const bool glyphChanged = a.ch != b.ch;
    const bool maskChanged = maskA != maskB;
    const bool dotsChanged = dotCount(maskA) != dotCount(maskB);
    const bool hasBgChanged = a.hasBg != b.hasBg;
    const bool changed = glyphChanged || hasBgChanged || fgDelta > 0 ||
                         bgDelta > 0;

    if (changed) ++delta.changedCells;
    if (glyphChanged) ++delta.glyphChanged;
    if (maskChanged) ++delta.maskChanged;
    if (dotsChanged) ++delta.dotCountChanged;
    if (hasBgChanged) ++delta.hasBgChanged;
    if (fgDelta > 0) ++delta.fgChanged;
    if (bgDelta > 0) ++delta.bgChanged;
    if (fgDelta > 2) ++delta.fgDeltaGt2;
    if (bgDelta > 2) ++delta.bgDeltaGt2;
    if (fgDelta > 8) ++delta.fgDeltaGt8;
    if (bgDelta > 8) ++delta.bgDeltaGt8;
    delta.fgDeltaSum += static_cast<uint64_t>(fgDelta);
    delta.bgDeltaSum += static_cast<uint64_t>(bgDelta);
    delta.bgLumaDeltaSum += std::abs(colorLuma(a.bg) - colorLuma(b.bg));
    delta.fgDeltaMax = std::max(delta.fgDeltaMax, fgDelta);
    delta.bgDeltaMax = std::max(delta.bgDeltaMax, bgDelta);
  }
  return delta;
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
         "dither_cells,edge_cells,"
         "signal_dampened,detail_boosted,edge_mask_fit,"
         "ink_coverage_compensated,"
         "ink_lifted,fg_temporal,bg_temporal,fullmask_bg_contrast\n";
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
      << stats.signalDampenCount << ',' << stats.detailBoostCount << ','
      << stats.edgeMaskFitCount << ','
      << stats.inkCoverageCompensationCount << ','
      << stats.inkLumaFloorCount << ',' << stats.fgTemporalBlendCount << ','
      << stats.bgTemporalBlendCount << ','
      << stats.fullMaskBgContrastCount << '\n';
}

void writeAblationHeader(std::ostream& out) {
  out << "fixture,variant,cells,changed_cells,changed_pct,glyph_changed,"
         "mask_changed,dot_count_changed,has_bg_changed,fg_changed,bg_changed,"
         "fg_delta_gt2,bg_delta_gt2,fg_delta_gt8,bg_delta_gt8,"
         "mean_fg_delta,mean_bg_delta,max_fg_delta,max_bg_delta,"
         "mean_bg_luma_delta\n";
}

void writeAblationRow(std::ostream& out, std::string_view fixture,
                      std::string_view variant, const OutputDelta& delta) {
  out << fixture << ',' << variant << ',' << delta.cellCount << ','
      << delta.changedCells << ',' << std::fixed << std::setprecision(2)
      << percent(delta.changedCells, delta.cellCount) << ','
      << delta.glyphChanged << ',' << delta.maskChanged << ','
      << delta.dotCountChanged << ',' << delta.hasBgChanged << ','
      << delta.fgChanged << ',' << delta.bgChanged << ','
      << delta.fgDeltaGt2 << ',' << delta.bgDeltaGt2 << ','
      << delta.fgDeltaGt8 << ',' << delta.bgDeltaGt8 << ','
      << std::setprecision(3)
      << (delta.cellCount == 0
              ? 0.0
              : static_cast<double>(delta.fgDeltaSum) /
                    static_cast<double>(delta.cellCount))
      << ','
      << (delta.cellCount == 0
              ? 0.0
              : static_cast<double>(delta.bgDeltaSum) /
                    static_cast<double>(delta.cellCount))
      << ',' << delta.fgDeltaMax << ',' << delta.bgDeltaMax << ','
      << (delta.cellCount == 0
              ? 0.0
              : delta.bgLumaDeltaSum / static_cast<double>(delta.cellCount))
      << '\n';
}

RenderedVariant renderVariant(const HarnessConfig& config,
                              std::string_view fixtureName,
                              const Variant& variant, const RgbaImage& image,
                              std::ostream& csv) {
  RenderedVariant rendered;
  ascii_debug::RenderOptions options;
  options.stageMask = variant.stageMask;
  options.resetHistory = true;
  options.tuning = variant.tuning;
  if (!renderAsciiArtFromRgbaDebug(image.pixels.data(), image.width,
                                   image.height, config.maxColumns,
                                   config.maxRows, rendered.art, options,
                                   &rendered.stats, true)) {
    fail("ASCII render failed");
  }

  std::filesystem::path fixtureDir = config.outputDir / std::string(fixtureName);
  std::filesystem::create_directories(fixtureDir);
  std::string base = std::string(variant.name);
  writeAnsi(fixtureDir / (base + ".ans"), rendered.art);
  writePlainText(fixtureDir / (base + ".txt"), rendered.art);
  writeAsciiRasterPng(fixtureDir / (base + ".png"), rendered.art);
  writeCsvRow(csv, fixtureName, variant.name, rendered.art, rendered.stats);

  std::cout << std::setw(16) << fixtureName << "  " << std::setw(22)
            << variant.name << "  bg="
            << std::fixed << std::setprecision(1)
            << percent(rendered.stats.bgCellCount, rendered.stats.cellCount)
            << "%  dots=" << std::setprecision(2)
            << averageDots(rendered.stats)
            << "  edgefit=" << rendered.stats.edgeMaskFitCount << '\n';
  return rendered;
}

void runHarness(const HarnessConfig& config) {
  std::filesystem::create_directories(config.outputDir);
  std::ofstream csv(config.outputDir / "summary.csv", std::ios::binary);
  if (!csv) fail("failed to open summary.csv");
  writeCsvHeader(csv);
  std::ofstream ablation(config.outputDir / "ablation.csv", std::ios::binary);
  if (!ablation) fail("failed to open ablation.csv");
  writeAblationHeader(ablation);

  std::vector<Variant> variantList = selectedVariants(config);
  for (std::string_view fixtureName : selectedFixtures(config)) {
    RgbaImage image = makeFixture(fixtureName, config.width, config.height);
    std::filesystem::path fixtureDir =
        config.outputDir / std::string(fixtureName);
    std::filesystem::create_directories(fixtureDir);
    writeSourcePng(fixtureDir / "source.png", image);
    RenderedVariant baseline =
        renderVariant(
            config, fixtureName,
            Variant{"current", ascii_debug::kAllStages, {}}, image, csv);
    for (const Variant& variant : variantList) {
      if (variant.name == "current") {
        writeAblationRow(ablation, fixtureName, variant.name,
                         compareArt(baseline.art, baseline.art));
        continue;
      }
      RenderedVariant rendered =
          renderVariant(config, fixtureName, variant, image, csv);
      writeAblationRow(ablation, fixtureName, variant.name,
                       compareArt(baseline.art, rendered.art));
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
