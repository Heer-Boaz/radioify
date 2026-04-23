#include "subtitles.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "subtitle_ass_bitmap_renderer.h"
#include "subtitle_caption_style.h"
#include "ui_helpers.h"
#include "unicode_display_width.h"

namespace playback_ascii_subtitles {
namespace {

struct SubtitleArea {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  bool hasVideoArt = false;
};

Color blendColor(const Color& src, const Color& dst, float alpha) {
  const float a = std::clamp(alpha, 0.0f, 1.0f);
  Color out;
  out.r = static_cast<uint8_t>(
      std::lround(static_cast<float>(src.r) * a +
                  static_cast<float>(dst.r) * (1.0f - a)));
  out.g = static_cast<uint8_t>(
      std::lround(static_cast<float>(src.g) * a +
                  static_cast<float>(dst.g) * (1.0f - a)));
  out.b = static_cast<uint8_t>(
      std::lround(static_cast<float>(src.b) * a +
                  static_cast<float>(dst.b) * (1.0f - a)));
  return out;
}

SubtitleArea computeSubtitleArea(const RenderInput& input) {
  SubtitleArea area;
  if (!input.art || input.width <= 0 || input.height <= input.artTop) {
    return area;
  }

  const int artWidth = std::min(input.art->width, input.width);
  const int artHeight = std::min(input.art->height, input.maxHeight);
  const int availableHeight = input.height - input.artTop;
  const int visibleArtHeight = std::min(artHeight, availableHeight);
  const bool canDrawArt =
      input.allowFrame && artWidth > 0 && visibleArtHeight > 0 &&
      input.art->cells.size() >=
          static_cast<size_t>(std::max(0, input.art->width)) *
              static_cast<size_t>(std::max(0, input.art->height));

  area.hasVideoArt = canDrawArt;
  area.x = canDrawArt ? std::max(0, (input.width - artWidth) / 2) : 0;
  area.y = input.artTop;
  area.width = canDrawArt ? artWidth : input.width;
  area.height = canDrawArt ? visibleArtHeight : (input.height - input.artTop);
  area.width = std::max(0, area.width);
  area.height = std::max(0, area.height);
  return area;
}

bool experimentalAssBitmapSubtitlesEnabled() {
  static const bool enabled = [] {
    std::optional<std::string> value =
        getEnvString("RADIOIFY_ASCII_ASS_BITMAP_SUBTITLES");
    if (!value || value->empty()) return false;
    std::string lower;
    lower.reserve(value->size());
    for (unsigned char ch : *value) {
      lower.push_back(static_cast<char>(std::tolower(ch)));
    }
    return lower == "1" || lower == "true" || lower == "yes" ||
           lower == "on" || lower == "ass" || lower == "bitmap";
  }();
  return enabled;
}

std::string escapeAssText(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char ch : text) {
    switch (ch) {
      case '\r':
        break;
      case '\n':
        out += "\\N";
        break;
      case '{':
      case '}':
        out.push_back(' ');
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::shared_ptr<const std::string> generatedPlainAssScript(
    const std::string& text, int canvasW, int canvasH) {
  struct Cache {
    std::string text;
    int width = 0;
    int height = 0;
    std::shared_ptr<const std::string> script;
  };
  static Cache cache;

  if (cache.script && cache.text == text && cache.width == canvasW &&
      cache.height == canvasH) {
    return cache.script;
  }

  const int fontSize = std::clamp(
      static_cast<int>(std::lround(static_cast<double>(canvasH) * 0.070)),
      16, std::max(16, canvasH / 5));
  const int marginV = std::max(8, static_cast<int>(std::lround(canvasH * 0.055)));
  const std::string escapedText = escapeAssText(text);

  std::string script;
  script.reserve(512 + escapedText.size());
  script += "[Script Info]\n";
  script += "ScriptType: v4.00+\n";
  script += "PlayResX: " + std::to_string(canvasW) + "\n";
  script += "PlayResY: " + std::to_string(canvasH) + "\n";
  script += "ScaledBorderAndShadow: yes\n\n";
  script += "[V4+ Styles]\n";
  script +=
      "Format: Name, Fontname, Fontsize, PrimaryColour, SecondaryColour, "
      "OutlineColour, BackColour, Bold, Italic, Underline, StrikeOut, ScaleX, "
      "ScaleY, Spacing, Angle, BorderStyle, Outline, Shadow, Alignment, "
      "MarginL, MarginR, MarginV, Encoding\n";
  script += "Style: Default,Arial," + std::to_string(fontSize) +
            ",&H00FFFFFF,&H000000FF,&H00000000,&H80000000,0,0,0,0,100,100,0,0,"
            "1,2,1,2,24,24," +
            std::to_string(marginV) + ",1\n\n";
  script += "[Events]\n";
  script += "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, "
            "Effect, Text\n";
  script += "Dialogue: 0,0:00:00.00,9:59:59.99,Default,,0,0,0,," +
            escapedText + "\n";

  cache.text = text;
  cache.width = canvasW;
  cache.height = canvasH;
  cache.script = std::make_shared<const std::string>(std::move(script));
  return cache.script;
}

uint8_t brailleMaskBit(int x, int y) {
  static constexpr uint8_t kBits[4][2] = {
      {0x01, 0x08},
      {0x02, 0x10},
      {0x04, 0x20},
      {0x40, 0x80},
  };
  return kBits[std::clamp(y, 0, 3)][std::clamp(x, 0, 1)];
}

Color cellBackground(const RenderInput& input, const SubtitleArea& area,
                     int cellX, int cellY) {
  if (area.hasVideoArt && input.art && cellX >= 0 && cellY >= 0 &&
      cellX < input.art->width && cellY < input.art->height) {
    const auto& cell =
        input.art->cells[static_cast<size_t>(cellY) * input.art->width + cellX];
    return cell.hasBg ? cell.bg : input.baseStyle.bg;
  }
  return input.baseStyle.bg;
}

bool renderAssBitmapSubtitles(const RenderInput& input) {
  SubtitleArea area = computeSubtitleArea(input);
  if (!input.screen || area.width <= 0 || area.height <= 0) return false;

  const int canvasW = area.width * 2;
  const int canvasH = area.height * 4;
  std::shared_ptr<const std::string> script = input.assScript;
  std::shared_ptr<const SubtitleFontAttachmentList> fonts = input.assFonts;
  int64_t clockUs = input.subtitleClockUs;
  if (!script || script->empty()) {
    if (input.subtitleText.empty()) return false;
    script = generatedPlainAssScript(input.subtitleText, canvasW, canvasH);
    fonts.reset();
    clockUs = 1000000;
  }

  static std::vector<uint8_t> canvas;
  SubtitleAssRenderResult result = renderAssSubtitlesToBgraCanvas(
      script, fonts, clockUs, canvasW, canvasH, &canvas);
  if (result.status != SubtitleAssRenderStatus::WithGlyph) {
    return false;
  }

  for (int cy = 0; cy < area.height; ++cy) {
    const int screenY = area.y + cy;
    if (screenY < input.artTop || screenY < 0 || screenY >= input.height) {
      continue;
    }
    for (int cx = 0; cx < area.width; ++cx) {
      uint8_t mask = 0;
      int alphaSum = 0;
      int colorWeight = 0;
      int bSum = 0;
      int gSum = 0;
      int rSum = 0;
      for (int py = 0; py < 4; ++py) {
        for (int px = 0; px < 2; ++px) {
          const int sx = cx * 2 + px;
          const int sy = cy * 4 + py;
          const size_t idx =
              (static_cast<size_t>(sy) * canvasW + static_cast<size_t>(sx)) * 4u;
          const int alpha = canvas[idx + 3];
          alphaSum += alpha;
          if (alpha >= 28) {
            mask |= brailleMaskBit(px, py);
            colorWeight += alpha;
            bSum += static_cast<int>(canvas[idx + 0]) * alpha;
            gSum += static_cast<int>(canvas[idx + 1]) * alpha;
            rSum += static_cast<int>(canvas[idx + 2]) * alpha;
          }
        }
      }
      if (mask == 0 || alphaSum < 44) continue;

      Color fg{255, 255, 255};
      if (colorWeight > 0) {
        fg.b = static_cast<uint8_t>(std::clamp(bSum / colorWeight, 0, 255));
        fg.g = static_cast<uint8_t>(std::clamp(gSum / colorWeight, 0, 255));
        fg.r = static_cast<uint8_t>(std::clamp(rSum / colorWeight, 0, 255));
      }

      const Color bg = cellBackground(input, area, cx, cy);
      const float avgAlpha =
          static_cast<float>(alphaSum) / (255.0f * 8.0f);
      if (avgAlpha < 0.85f) {
        fg = blendColor(fg, bg, std::clamp(avgAlpha * 1.35f, 0.0f, 1.0f));
      }

      const int screenX = area.x + cx;
      if (screenX < 0 || screenX >= input.width) continue;
      const wchar_t ch = static_cast<wchar_t>(0x2800u + mask);
      input.screen->writeChar(screenX, screenY, ch, Style{fg, bg});
    }
  }
  return true;
}

struct StyledRun {
  std::string text;
  bool hasPrimaryColor = false;
  Color primaryColor{255, 255, 255};
  float primaryAlpha = 1.0f;
  bool hasBackColor = false;
  Color backColor{0, 0, 0};
  float backAlpha = 0.55f;
};

struct StyledLine {
  std::vector<StyledRun> runs;
  int displayWidth = 0;
};

struct StyledGlyph {
  std::string text;
  int width = 0;
  bool trimSpace = false;
  StyledRun style;
};

bool sameStyledRunStyle(const StyledRun& a, const StyledRun& b) {
  return a.hasPrimaryColor == b.hasPrimaryColor &&
         a.primaryColor.r == b.primaryColor.r &&
         a.primaryColor.g == b.primaryColor.g &&
         a.primaryColor.b == b.primaryColor.b &&
         std::abs(a.primaryAlpha - b.primaryAlpha) < 0.001f &&
         a.hasBackColor == b.hasBackColor && a.backColor.r == b.backColor.r &&
         a.backColor.g == b.backColor.g && a.backColor.b == b.backColor.b &&
         std::abs(a.backAlpha - b.backAlpha) < 0.001f;
}

void appendStyledRun(std::vector<StyledRun>* runs, const StyledRun& style,
                     std::string_view text) {
  if (!runs || text.empty()) return;
  if (!runs->empty() && sameStyledRunStyle(runs->back(), style)) {
    runs->back().text.append(text);
    return;
  }
  StyledRun run = style;
  run.text.assign(text);
  runs->push_back(std::move(run));
}

StyledRun styledRunFromCueRun(const WindowUiState::SubtitleCue::TextRun& run) {
  StyledRun out;
  out.hasPrimaryColor = run.hasPrimaryColor;
  out.primaryColor = run.primaryColor;
  out.primaryAlpha = run.primaryAlpha;
  out.hasBackColor = run.hasBackColor;
  out.backColor = run.backColor;
  out.backAlpha = run.backAlpha;
  return out;
}

StyledRun baseStyledRunFromCue(const WindowUiState::SubtitleCue& cue) {
  StyledRun out;
  out.hasPrimaryColor = cue.hasPrimaryColor;
  out.primaryColor = cue.primaryColor;
  out.primaryAlpha = cue.primaryAlpha;
  out.hasBackColor = cue.hasBackColor;
  out.backColor = cue.backColor;
  out.backAlpha = cue.backAlpha;
  return out;
}

StyledRun styleOnlyRun(const StyledRun& run) {
  StyledRun out;
  out.hasPrimaryColor = run.hasPrimaryColor;
  out.primaryColor = run.primaryColor;
  out.primaryAlpha = run.primaryAlpha;
  out.hasBackColor = run.hasBackColor;
  out.backColor = run.backColor;
  out.backAlpha = run.backAlpha;
  return out;
}

std::vector<StyledRun> cueStyledRuns(const WindowUiState::SubtitleCue& cue) {
  std::vector<StyledRun> runs;
  if (!cue.textRuns.empty()) {
    runs.reserve(cue.textRuns.size());
    for (const auto& cueRun : cue.textRuns) {
      appendStyledRun(&runs, styledRunFromCueRun(cueRun), cueRun.text);
    }
    return runs;
  }

  StyledRun run = baseStyledRunFromCue(cue);
  appendStyledRun(&runs, run, cue.text);
  return runs;
}

void trimStyledRuns(std::vector<StyledRun>* runs) {
  if (!runs) return;
  while (!runs->empty()) {
    std::string& text = runs->front().text;
    size_t begin = 0;
    while (begin < text.size() &&
           (text[begin] == ' ' || text[begin] == '\t' ||
            text[begin] == '\r')) {
      ++begin;
    }
    if (begin > 0) text.erase(0, begin);
    if (!text.empty()) break;
    runs->erase(runs->begin());
  }
  while (!runs->empty()) {
    std::string& text = runs->back().text;
    while (!text.empty() &&
           (text.back() == ' ' || text.back() == '\t' ||
            text.back() == '\r')) {
      text.pop_back();
    }
    if (!text.empty()) break;
    runs->pop_back();
  }
}

std::vector<std::vector<StyledRun>> splitStyledRunsByNewline(
    const std::vector<StyledRun>& runs) {
  std::vector<std::vector<StyledRun>> lines(1);
  for (const StyledRun& run : runs) {
    size_t start = 0;
    while (start <= run.text.size()) {
      size_t end = run.text.find('\n', start);
      std::string_view piece =
          (end == std::string::npos)
              ? std::string_view(run.text).substr(start)
              : std::string_view(run.text).substr(start, end - start);
      appendStyledRun(&lines.back(), run, piece);
      if (end == std::string::npos) break;
      lines.emplace_back();
      start = end + 1;
    }
  }
  for (auto& line : lines) {
    trimStyledRuns(&line);
  }
  return lines;
}

std::vector<StyledGlyph> flattenStyledRuns(
    const std::vector<StyledRun>& runs) {
  std::vector<StyledGlyph> glyphs;
  for (const StyledRun& run : runs) {
    const StyledRun runStyle = styleOnlyRun(run);
    size_t offset = 0;
    char32_t codepoint = 0;
    size_t glyphStart = 0;
    size_t glyphEnd = 0;
    while (utf8DecodeCodepoint(run.text, &offset, &codepoint, &glyphStart,
                               &glyphEnd)) {
      int width = unicodeDisplayWidth(codepoint);
      if (width <= 0) continue;
      StyledGlyph glyph;
      glyph.text = run.text.substr(glyphStart, glyphEnd - glyphStart);
      glyph.width = width;
      glyph.trimSpace = codepoint == U' ' || codepoint == U'\t' ||
                        codepoint == U'\r';
      glyph.style = runStyle;
      glyphs.push_back(std::move(glyph));
    }
  }
  return glyphs;
}

StyledLine makeStyledLineFromGlyphs(const std::vector<StyledGlyph>& glyphs,
                                    size_t begin, size_t end) {
  while (begin < end && glyphs[begin].trimSpace) ++begin;
  while (end > begin && glyphs[end - 1].trimSpace) --end;

  StyledLine line;
  for (size_t i = begin; i < end; ++i) {
    appendStyledRun(&line.runs, glyphs[i].style, glyphs[i].text);
    line.displayWidth += glyphs[i].width;
  }
  return line;
}

void appendWrappedStyledRuns(const std::vector<StyledRun>& rawRuns,
                             int maxChars, int maxLines,
                             std::vector<StyledLine>* lines) {
  if (!lines || maxChars <= 0 || maxLines <= 0 ||
      static_cast<int>(lines->size()) >= maxLines) {
    return;
  }
  std::vector<StyledGlyph> glyphs = flattenStyledRuns(rawRuns);
  size_t start = 0;
  while (start < glyphs.size() && static_cast<int>(lines->size()) < maxLines) {
    while (start < glyphs.size() && glyphs[start].trimSpace) ++start;
    if (start >= glyphs.size()) break;

    size_t end = start;
    size_t lastBreak = std::string::npos;
    int width = 0;
    int widthAtBreak = 0;
    while (end < glyphs.size() && width + glyphs[end].width <= maxChars) {
      width += glyphs[end].width;
      if (glyphs[end].trimSpace) {
        lastBreak = end;
        widthAtBreak = width - glyphs[end].width;
      }
      ++end;
    }

    if (end < glyphs.size() && lastBreak != std::string::npos &&
        widthAtBreak >= maxChars / 3) {
      end = lastBreak;
    }
    if (end <= start) {
      end = start + 1;
    }

    StyledLine line = makeStyledLineFromGlyphs(glyphs, start, end);
    if (!line.runs.empty()) lines->push_back(std::move(line));
    start = end;
  }
}

std::vector<StyledLine> wrapStyledSubtitleText(
    const WindowUiState::SubtitleCue& cue, int maxChars, int maxLines) {
  std::vector<StyledLine> lines;
  if (maxChars <= 0 || maxLines <= 0) return lines;

  std::vector<StyledRun> runs = cueStyledRuns(cue);
  std::vector<std::vector<StyledRun>> rawLines = splitStyledRunsByNewline(runs);
  for (const auto& rawLine : rawLines) {
    if (static_cast<int>(lines.size()) >= maxLines) break;
    if (rawLine.empty()) continue;
    appendWrappedStyledRuns(rawLine, maxChars, maxLines, &lines);
  }
  return lines;
}

enum class CueVerticalAnchor { Bottom, Middle, Top };

struct CueBlock {
  std::string text;
  std::vector<StyledLine> lines;
  int alignment = 2;
  int layer = 0;
  int order = 0;
  bool hasPosition = false;
  float posX = 0.5f;
  float posY = 0.9f;
  bool hasClip = false;
  bool inverseClip = false;
  int clipLeft = 0;
  int clipTop = 0;
  int clipRight = 0;
  int clipBottom = 0;
  int marginL = 0;
  int marginR = 0;
  int marginV = 0;
  int contentWidth = 0;
  int width = 0;
  int height = 0;
  int x = 0;
  int y = 0;
  bool hasTransform = false;
  bool assStyled = false;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool hasPrimaryColor = false;
  Color primaryColor{255, 255, 255};
  float primaryAlpha = 1.0f;
  bool hasBackColor = false;
  Color backColor{0, 0, 0};
  float backAlpha = 0.55f;
};

struct SubtitleCell {
  bool occupied = false;
  bool continuation = false;
  std::string text;
  int width = 1;
  Style style{};
};

struct SubtitleCellCanvas {
  int width = 0;
  int height = 0;
  std::vector<SubtitleCell> cells;

  void reset(int newWidth, int newHeight) {
    width = std::max(0, newWidth);
    height = std::max(0, newHeight);
    cells.clear();
    cells.resize(static_cast<size_t>(width) * static_cast<size_t>(height));
  }

  SubtitleCell* cellAt(int x, int y) {
    if (x < 0 || y < 0 || x >= width || y >= height) return nullptr;
    return &cells[static_cast<size_t>(y) * static_cast<size_t>(width) +
                  static_cast<size_t>(x)];
  }
};

int cueHorizontalAnchor(int alignment) {
  alignment = std::clamp(alignment, 1, 9);
  return (alignment - 1) % 3;
}

CueVerticalAnchor cueVerticalAnchor(int alignment) {
  alignment = std::clamp(alignment, 1, 9);
  if (alignment >= 7) return CueVerticalAnchor::Top;
  if (alignment >= 4) return CueVerticalAnchor::Middle;
  return CueVerticalAnchor::Bottom;
}

int cueLineMax(const WindowUiState::SubtitleCue& cue, int availableWidth) {
  const float xScale =
      std::clamp(cue.sizeScale * cue.scaleX, 0.75f, 1.80f);
  return std::max(6, static_cast<int>(
                         std::floor(static_cast<float>(availableWidth) /
                                    std::sqrt(xScale))));
}

int cueMaxLines(const SubtitleArea& area, bool overlayVisible) {
  const int byHeight = std::max(2, area.height / 3);
  return std::clamp(byHeight, 2, overlayVisible ? 4 : 6);
}

int lineDisplayWidth(const std::vector<StyledLine>& lines) {
  int width = 0;
  for (const StyledLine& line : lines) {
    width = std::max(width, line.displayWidth);
  }
  return width;
}

int normalizedPixelCoordToEdge(float value, int cellCount);

bool makeCueBlock(const WindowUiState::SubtitleCue& cue, const SubtitleArea& area,
                  bool overlayVisible, int order, CueBlock* out) {
  if (!out || cue.text.empty() || area.width <= 2 || area.height <= 0) {
    return false;
  }

  CueBlock block;
  block.text = cue.text;
  block.alignment = std::clamp(cue.alignment, 1, 9);
  block.layer = cue.layer;
  block.order = order;
  block.hasPosition = cue.hasPosition;
  block.posX = std::clamp(cue.posX, 0.0f, 1.0f);
  block.posY = std::clamp(cue.posY, 0.0f, 1.0f);
  block.hasTransform = cue.hasTransform;
  block.hasClip = cue.hasClip;
  block.inverseClip = cue.inverseClip;
  if (block.hasClip) {
    const int x1 = normalizedPixelCoordToEdge(cue.clipX1, area.width);
    const int y1 = normalizedPixelCoordToEdge(cue.clipY1, area.height);
    const int x2 = normalizedPixelCoordToEdge(cue.clipX2, area.width);
    const int y2 = normalizedPixelCoordToEdge(cue.clipY2, area.height);
    block.clipLeft = area.x + std::min(x1, x2);
    block.clipTop = area.y + std::min(y1, y2);
    block.clipRight = area.x + std::max(x1, x2);
    block.clipBottom = area.y + std::max(y1, y2);
  }
  block.marginL =
      std::clamp(static_cast<int>(std::lround(cue.marginLNorm * area.width)),
                 0, std::max(0, area.width - 1));
  block.marginR =
      std::clamp(static_cast<int>(std::lround(cue.marginRNorm * area.width)),
                 0, std::max(0, area.width - 1));
  block.marginV =
      std::clamp(static_cast<int>(std::lround(cue.marginVNorm * area.height)),
                 0, std::max(0, area.height / 2));
  block.assStyled = cue.assStyled;
  block.bold = cue.bold;
  block.italic = cue.italic;
  block.underline = cue.underline;
  block.hasPrimaryColor = cue.hasPrimaryColor;
  block.primaryColor = cue.primaryColor;
  block.primaryAlpha = cue.primaryAlpha;
  block.hasBackColor = cue.hasBackColor;
  block.backColor = cue.backColor;
  block.backAlpha = cue.backAlpha;
  if (!cue.textRuns.empty()) {
    block.hasPrimaryColor = false;
    block.primaryAlpha = 1.0f;
    block.hasBackColor = false;
    block.backAlpha = 0.55f;
  }

  int availableWidth = area.width - block.marginL - block.marginR - 2;
  if (block.hasPosition) availableWidth = area.width - 2;
  availableWidth = std::max(6, availableWidth);
  const int maxChars = cueLineMax(cue, availableWidth);
  block.lines = wrapStyledSubtitleText(cue, maxChars,
                                       cueMaxLines(area, overlayVisible));
  if (block.lines.empty()) return false;

  block.contentWidth = std::max(1, lineDisplayWidth(block.lines));
  block.width = std::min(area.width, block.contentWidth + 2);
  block.height = static_cast<int>(block.lines.size());
  *out = std::move(block);
  return true;
}

WindowUiState::SubtitleCue plainCueFromText(const std::string& text) {
  WindowUiState::SubtitleCue cue;
  cue.text = text;
  cue.rawText = text;
  cue.alignment = 2;
  return cue;
}

bool rowsBlocked(const std::vector<uint8_t>& occupied, int y, int height) {
  if (height <= 0 || y < 0 ||
      y + height > static_cast<int>(occupied.size())) {
    return true;
  }
  for (int row = y; row < y + height; ++row) {
    if (occupied[static_cast<size_t>(row)] != 0) return true;
  }
  return false;
}

void markRows(std::vector<uint8_t>* occupied, int y, int height) {
  if (!occupied || height <= 0) return;
  const int begin = std::max(0, y);
  const int end =
      std::min(static_cast<int>(occupied->size()), y + height);
  for (int row = begin; row < end; ++row) {
    (*occupied)[static_cast<size_t>(row)] = 1;
  }
}

int findBottomStackY(const std::vector<uint8_t>& occupied, int startY,
                     int minY, int height) {
  for (int y = startY; y >= minY; --y) {
    if (!rowsBlocked(occupied, y, height)) return y;
  }
  return minY;
}

int findTopStackY(const std::vector<uint8_t>& occupied, int startY, int maxY,
                  int height) {
  for (int y = startY; y <= maxY; ++y) {
    if (!rowsBlocked(occupied, y, height)) return y;
  }
  return maxY;
}

int findMiddleStackY(const std::vector<uint8_t>& occupied, int startY,
                     int minY, int maxY, int height) {
  const int maxOffset = std::max(startY - minY, maxY - startY);
  for (int offset = 0; offset <= maxOffset; ++offset) {
    const int up = startY - offset;
    if (up >= minY && !rowsBlocked(occupied, up, height)) return up;
    const int down = startY + offset;
    if (down <= maxY && !rowsBlocked(occupied, down, height)) return down;
  }
  return std::clamp(startY, minY, maxY);
}

int clampBlockX(int x, const SubtitleArea& area, int width) {
  return std::clamp(x, area.x, std::max(area.x, area.x + area.width - width));
}

int normalizedPixelCoordToCell(float value, int cellCount) {
  if (cellCount <= 1) return 0;
  return std::clamp(static_cast<int>(std::floor(
                        std::clamp(value, 0.0f, 1.0f) *
                        static_cast<float>(cellCount))),
                    0, cellCount - 1);
}

int normalizedPixelCoordToEdge(float value, int cellCount) {
  if (cellCount <= 0) return 0;
  return std::clamp(static_cast<int>(std::lround(
                        std::clamp(value, 0.0f, 1.0f) *
                        static_cast<float>(cellCount))),
                    0, cellCount);
}

int clampBlockY(int y, const SubtitleArea& area, int height, int bottomLimit) {
  const int maxY = std::max(area.y, std::min(area.y + area.height - height,
                                            bottomLimit - height + 1));
  return std::clamp(y, area.y, maxY);
}

void positionExplicitBlock(CueBlock* block, const SubtitleArea& area,
                           int bottomLimit) {
  if (!block) return;
  const int anchorX =
      area.x + normalizedPixelCoordToCell(block->posX, area.width);
  const int anchorY =
      area.y + normalizedPixelCoordToCell(block->posY, area.height);
  switch (cueHorizontalAnchor(block->alignment)) {
    case 0:
      block->x = anchorX;
      break;
    case 2:
      block->x = anchorX - block->width + 1;
      break;
    default:
      block->x = anchorX - block->width / 2;
      break;
  }
  switch (cueVerticalAnchor(block->alignment)) {
    case CueVerticalAnchor::Top:
      block->y = anchorY;
      break;
    case CueVerticalAnchor::Middle:
      block->y = anchorY - block->height / 2;
      break;
    case CueVerticalAnchor::Bottom:
      block->y = anchorY - block->height + 1;
      break;
  }
  block->x = clampBlockX(block->x, area, block->width);
  block->y = clampBlockY(block->y, area, block->height, bottomLimit);
}

void positionFlowBlockHorizontal(CueBlock* block, const SubtitleArea& area) {
  if (!block) return;
  const int left = area.x + block->marginL;
  const int right = area.x + area.width - block->marginR;
  const int availableWidth = std::max(block->width, right - left);
  switch (cueHorizontalAnchor(block->alignment)) {
    case 0:
      block->x = left;
      break;
    case 2:
      block->x = right - block->width;
      break;
    default:
      block->x = left + (availableWidth - block->width) / 2;
      break;
  }
  block->x = clampBlockX(block->x, area, block->width);
}

void positionFlowBlockUnstacked(CueBlock* block, const SubtitleArea& area,
                                int bottomLimit) {
  if (!block) return;
  positionFlowBlockHorizontal(block, area);

  const int minY = area.y;
  const int maxY = std::max(area.y, bottomLimit - block->height + 1);
  switch (cueVerticalAnchor(block->alignment)) {
    case CueVerticalAnchor::Top: {
      block->y = std::min(maxY, area.y + block->marginV);
      break;
    }
    case CueVerticalAnchor::Middle: {
      block->y =
          std::clamp(area.y + (area.height - block->height) / 2, minY, maxY);
      break;
    }
    case CueVerticalAnchor::Bottom: {
      block->y =
          std::clamp(bottomLimit - block->marginV - block->height + 1, minY,
                     maxY);
      break;
    }
  }
  block->y = clampBlockY(block->y, area, block->height, bottomLimit);
}

void positionFlowBlock(CueBlock* block, const SubtitleArea& area,
                       int bottomLimit, std::vector<uint8_t>* occupied) {
  if (!block || !occupied) return;
  positionFlowBlockHorizontal(block, area);

  const int minY = area.y;
  const int maxY = std::max(area.y, bottomLimit - block->height + 1);
  switch (cueVerticalAnchor(block->alignment)) {
    case CueVerticalAnchor::Top: {
      const int desired = std::min(maxY, area.y + block->marginV);
      block->y = findTopStackY(*occupied, desired, maxY, block->height);
      break;
    }
    case CueVerticalAnchor::Middle: {
      const int desired =
          std::clamp(area.y + (area.height - block->height) / 2, minY, maxY);
      block->y =
          findMiddleStackY(*occupied, desired, minY, maxY, block->height);
      break;
    }
    case CueVerticalAnchor::Bottom: {
      const int desired =
          std::clamp(bottomLimit - block->marginV - block->height + 1, minY,
                     maxY);
      block->y = findBottomStackY(*occupied, desired, minY, block->height);
      break;
    }
  }
  block->y = clampBlockY(block->y, area, block->height, bottomLimit);
  markRows(occupied, block->y, block->height);
}

bool sameSubtitleOverlayText(const CueBlock& a, const CueBlock& b) {
  return !a.text.empty() && a.text == b.text;
}

bool sameSubtitleOverlayPlacement(const CueBlock& a, const CueBlock& b) {
  if (a.alignment != b.alignment || a.hasPosition != b.hasPosition) {
    return false;
  }
  if (a.hasPosition) {
    return std::abs(a.posX - b.posX) <= 0.01f &&
           std::abs(a.posY - b.posY) <= 0.01f;
  }
  return std::abs(a.marginL - b.marginL) <= 1 &&
         std::abs(a.marginR - b.marginR) <= 1 &&
         std::abs(a.marginV - b.marginV) <= 1;
}

bool shouldOverlaySubtitleBlock(const CueBlock& previous,
                                const CueBlock& current) {
  return sameSubtitleOverlayText(previous, current) &&
         sameSubtitleOverlayPlacement(previous, current);
}

bool sameStyle(const Style& a, const Style& b) {
  return a.fg.r == b.fg.r && a.fg.g == b.fg.g && a.fg.b == b.fg.b &&
         a.bg.r == b.bg.r && a.bg.g == b.bg.g && a.bg.b == b.bg.b;
}

bool cueBlockAllowsCell(const CueBlock& block, int x, int y) {
  if (!block.hasClip) return true;
  const bool inside = x >= block.clipLeft && x < block.clipRight &&
                      y >= block.clipTop && y < block.clipBottom;
  return block.inverseClip ? !inside : inside;
}

void writeSubtitleCell(SubtitleCellCanvas* canvas, int x, int y,
                       const CueBlock& block, std::string_view text, int width,
                       const Style& style) {
  if (!canvas || text.empty() || width <= 0) return;
  if (!cueBlockAllowsCell(block, x, y)) return;
  for (int dx = 1; dx < width; ++dx) {
    if (!cueBlockAllowsCell(block, x + dx, y)) return;
  }
  SubtitleCell* cell = canvas->cellAt(x, y);
  if (!cell) return;

  cell->occupied = true;
  cell->continuation = false;
  cell->text.assign(text);
  cell->width = std::min(width, std::max(1, canvas->width - x));
  cell->style = style;

  for (int dx = 1; dx < cell->width; ++dx) {
    SubtitleCell* continuation = canvas->cellAt(x + dx, y);
    if (!continuation) break;
    continuation->occupied = true;
    continuation->continuation = true;
    continuation->text.clear();
    continuation->width = 0;
    continuation->style = style;
  }
}

void writeSubtitleSpaceRun(SubtitleCellCanvas* canvas, int x, int y, int len,
                           const CueBlock& block, const Style& style) {
  if (!canvas || len <= 0) return;
  const int begin = std::max(0, x);
  const int end = std::min(canvas->width, x + len);
  if (y < 0 || y >= canvas->height || begin >= end) return;
  for (int cx = begin; cx < end; ++cx) {
    writeSubtitleCell(canvas, cx, y, block, " ", 1, style);
  }
}

Color brightenColor(Color color, float amount) {
  amount = std::clamp(amount, 0.0f, 1.0f);
  color.r = static_cast<uint8_t>(
      std::lround(color.r + (255 - color.r) * amount));
  color.g = static_cast<uint8_t>(
      std::lround(color.g + (255 - color.g) * amount));
  color.b = static_cast<uint8_t>(
      std::lround(color.b + (255 - color.b) * amount));
  return color;
}

void composeCueBlock(const RenderInput& input, SubtitleCellCanvas* canvas,
                     const CueBlock& block, const SubtitleArea& area,
                     const Style& textStyle, const Style& boxStyle) {
  if (!canvas) return;
  constexpr int kPadX = 1;
  const int contentStartX = block.x + kPadX;
  const int contentWidth = std::max(1, block.width - kPadX * 2);

  for (int lineIndex = 0; lineIndex < block.height; ++lineIndex) {
    const int y = block.y + lineIndex;
    if (y < area.y || y >= area.y + area.height || y < 0 ||
        y >= input.height) {
      continue;
    }

    const int boxX = std::max(0, block.x);
    const int boxW = std::min(input.width - boxX, block.width);
    if (boxW > 0) {
      writeSubtitleSpaceRun(canvas, boxX, y, boxW, block, boxStyle);
    }

    const StyledLine& line = block.lines[static_cast<size_t>(lineIndex)];
    const int lineWidth = std::min(line.displayWidth, contentWidth);
    int x = contentStartX;
    switch (cueHorizontalAnchor(block.alignment)) {
      case 2:
        x = contentStartX + std::max(0, contentWidth - lineWidth);
        break;
      case 1:
        x = contentStartX + std::max(0, (contentWidth - lineWidth) / 2);
        break;
      default:
        break;
    }
    x = std::clamp(x, 0, std::max(0, input.width - lineWidth));

    int cursorX = x;
    int writtenWidth = 0;
    for (const StyledRun& run : line.runs) {
      if (writtenWidth >= contentWidth) break;
      std::string text = run.text;
      int runWidth = utf8DisplayWidth(text);
      const int remaining = contentWidth - writtenWidth;
      if (runWidth > remaining) {
        text = utf8TakeDisplayWidth(text, remaining);
        runWidth = utf8DisplayWidth(text);
      }
      if (text.empty() || runWidth <= 0) continue;

      Style runStyle = textStyle;
      Color runBg = boxStyle.bg;
      if (run.hasBackColor) {
        runBg = blendColor(run.backColor, input.baseStyle.bg,
                           std::clamp(run.backAlpha, 0.45f, 0.92f));
      }
      runStyle.bg = runBg;
      if (run.hasPrimaryColor) {
        runStyle.fg = blendColor(run.primaryColor, runBg, run.primaryAlpha);
      } else if (run.primaryAlpha < 0.999f) {
        runStyle.fg = blendColor(textStyle.fg, runBg, run.primaryAlpha);
      }
      size_t offset = 0;
      char32_t codepoint = 0;
      size_t glyphStart = 0;
      size_t glyphEnd = 0;
      int glyphCursorX = cursorX;
      while (utf8DecodeCodepoint(text, &offset, &codepoint, &glyphStart,
                                 &glyphEnd)) {
        const int glyphWidth = unicodeDisplayWidth(codepoint);
        if (glyphWidth <= 0) continue;
        if (glyphCursorX + glyphWidth > block.x + block.width - kPadX) break;
        writeSubtitleCell(
            canvas, glyphCursorX, y, block,
            std::string_view(text.data() + glyphStart, glyphEnd - glyphStart),
            glyphWidth, runStyle);
        glyphCursorX += glyphWidth;
      }
      cursorX += runWidth;
      writtenWidth += runWidth;
    }
  }
}

void flushSubtitleCanvas(const RenderInput& input,
                         const SubtitleCellCanvas& canvas) {
  if (!input.screen || canvas.width <= 0 || canvas.height <= 0) return;
  for (int y = 0; y < canvas.height; ++y) {
    int x = 0;
    while (x < canvas.width) {
      const SubtitleCell& cell =
          canvas.cells[static_cast<size_t>(y) * static_cast<size_t>(canvas.width) +
                       static_cast<size_t>(x)];
      if (!cell.occupied || cell.continuation || cell.text.empty()) {
        ++x;
        continue;
      }
      if (cell.text == " " && cell.width == 1) {
        int end = x + 1;
        while (end < canvas.width) {
          const SubtitleCell& next =
              canvas.cells[static_cast<size_t>(y) *
                               static_cast<size_t>(canvas.width) +
                           static_cast<size_t>(end)];
          if (!next.occupied || next.continuation || next.text != " " ||
              next.width != 1 || !sameStyle(next.style, cell.style)) {
            break;
          }
          ++end;
        }
        input.screen->writeRun(x, y, end - x, L' ', cell.style);
        x = end;
        continue;
      }
      input.screen->writeText(x, y, cell.text, cell.style);
      x += std::max(1, cell.width);
    }
  }
}

std::pair<Style, Style> cueCaptionStyles(const RenderInput& input,
                                         const CueBlock& block,
                                         const CaptionStyleProfile& profile) {
  Color targetText{profile.textR, profile.textG, profile.textB};
  float textAlpha = profile.textAlpha;
  if (block.hasPrimaryColor) {
    targetText = block.primaryColor;
  }
  if (block.hasPrimaryColor || block.primaryAlpha < 0.999f) {
    textAlpha = block.primaryAlpha;
  }

  Color targetBg{profile.backgroundR, profile.backgroundG,
                 profile.backgroundB};
  float boxAlpha = std::max(profile.backgroundAlpha, 0.72f);
  if (block.hasBackColor) {
    targetBg = block.backColor;
    boxAlpha = std::clamp(block.backAlpha, 0.45f, 0.92f);
  }

  const Color blendedBg = blendColor(targetBg, input.baseStyle.bg, boxAlpha);
  Color blendedText = blendColor(targetText, blendedBg, textAlpha);
  if (!block.hasPrimaryColor) {
    blendedText = brightenColor(blendedText, 0.10f);
  }

  Style boxStyle = input.baseStyle;
  boxStyle.fg = blendedBg;
  boxStyle.bg = blendedBg;

  Style textStyle = input.accentStyle;
  textStyle.fg = blendedText;
  textStyle.bg = blendedBg;
  return {textStyle, boxStyle};
}

void renderReadableCueSubtitles(const RenderInput& input) {
  if (!input.screen || input.width <= 2 || input.height <= input.artTop + 1) {
    return;
  }

  SubtitleArea area = computeSubtitleArea(input);
  if (area.width <= 2 || area.height <= 0) return;

  std::vector<CueBlock> blocks;
  const int activeCueCount =
      input.subtitleCues ? static_cast<int>(input.subtitleCues->size()) : 0;
  blocks.reserve(static_cast<size_t>(std::max(1, activeCueCount)));
  int order = 0;
  if (input.subtitleCues && !input.subtitleCues->empty()) {
    for (const WindowUiState::SubtitleCue& cue : *input.subtitleCues) {
      CueBlock block;
      if (makeCueBlock(cue, area, input.overlayVisible, order, &block)) {
        blocks.push_back(std::move(block));
      }
      ++order;
    }
  } else if (!input.subtitleText.empty()) {
    WindowUiState::SubtitleCue cue = plainCueFromText(input.subtitleText);
    CueBlock block;
    if (makeCueBlock(cue, area, input.overlayVisible, order, &block)) {
      blocks.push_back(std::move(block));
    }
  }
  if (blocks.empty()) return;

  std::stable_sort(blocks.begin(), blocks.end(),
                   [](const CueBlock& a, const CueBlock& b) {
                     if (a.layer != b.layer) return a.layer < b.layer;
                     if (a.hasPosition != b.hasPosition) {
                       return !a.hasPosition && b.hasPosition;
                     }
                     return a.order < b.order;
                   });

  const int artBottomY = area.hasVideoArt ? (area.y + area.height - 1)
                                          : (input.height - 1);
  const int overlayTopY =
      input.overlayVisible
          ? (input.height - std::max(5, input.overlayReservedLines))
          : input.height;
  int bottomLimit = std::min(artBottomY, overlayTopY - 1);
  bottomLimit = std::clamp(bottomLimit, area.y, area.y + area.height - 1);
  const int explicitPositionBottomLimit = area.y + area.height - 1;

  std::vector<uint8_t> occupied(static_cast<size_t>(input.height), 0);
  std::vector<size_t> positionedBlocks;
  positionedBlocks.reserve(blocks.size());
  for (size_t blockIndex = 0; blockIndex < blocks.size(); ++blockIndex) {
    CueBlock& block = blocks[blockIndex];
    bool overlaid = false;
    for (auto it = positionedBlocks.rbegin(); it != positionedBlocks.rend();
         ++it) {
      const CueBlock& previous = blocks[*it];
      if (!shouldOverlaySubtitleBlock(previous, block)) continue;
      block.x = previous.x;
      block.y = previous.y;
      overlaid = true;
      break;
    }

    if (!overlaid) {
      if (block.hasPosition) {
        positionExplicitBlock(&block, area, explicitPositionBottomLimit);
        markRows(&occupied, block.y, block.height);
      } else if (block.hasTransform) {
        positionFlowBlockUnstacked(&block, area, bottomLimit);
      } else {
        positionFlowBlock(&block, area, bottomLimit, &occupied);
      }
    }
    positionedBlocks.push_back(blockIndex);
  }

  const CaptionStyleProfile captionStyle = getWindowsCaptionStyleProfile();

  SubtitleCellCanvas canvas;
  canvas.reset(input.width, input.height);
  for (const CueBlock& block : blocks) {
    const auto [textStyle, boxStyle] =
        cueCaptionStyles(input, block, captionStyle);
    composeCueBlock(input, &canvas, block, area, textStyle, boxStyle);
  }
  flushSubtitleCanvas(input, canvas);
}

}  // namespace

void renderAsciiSubtitles(const RenderInput& input) {
  if (experimentalAssBitmapSubtitlesEnabled() &&
      renderAssBitmapSubtitles(input)) {
    return;
  }
  renderReadableCueSubtitles(input);
}

}  // namespace playback_ascii_subtitles
