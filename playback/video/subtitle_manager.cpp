#include "subtitle_manager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <utility>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#include "runtime_helpers.h"
#include "subtitle_font_attachments.h"
#include "ui_helpers.h"

namespace {

std::string toLowerAscii(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

bool startsWith(const std::string& value, const std::string& prefix) {
  if (prefix.size() > value.size()) return false;
  return std::equal(prefix.begin(), prefix.end(), value.begin());
}

bool startsWithAsciiNoCase(std::string_view value, size_t offset,
                           std::string_view prefix) {
  if (offset > value.size() || prefix.size() > value.size() - offset) {
    return false;
  }
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[offset + i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i]))) {
      return false;
    }
  }
  return true;
}

std::string_view trimAsciiView(std::string_view v) {
  size_t begin = 0;
  while (begin < v.size() &&
         std::isspace(static_cast<unsigned char>(v[begin])) != 0) {
    ++begin;
  }
  size_t end = v.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(v[end - 1])) != 0) {
    --end;
  }
  return v.substr(begin, end - begin);
}

std::string trimAscii(std::string_view v) {
  std::string_view t = trimAsciiView(v);
  return std::string(t.begin(), t.end());
}

bool isDigitsOnly(std::string_view v) {
  if (v.empty()) return false;
  for (char ch : v) {
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) {
      return false;
    }
  }
  return true;
}

bool parseNonNegativeInt(std::string_view v, int* out) {
  if (!out || v.empty()) return false;
  int value = 0;
  for (char ch : v) {
    if (std::isdigit(static_cast<unsigned char>(ch)) == 0) return false;
    int digit = ch - '0';
    if (value > ((std::numeric_limits<int>::max)() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  *out = value;
  return true;
}

bool parseSignedInt(std::string_view v, int* out) {
  if (!out) return false;
  std::string s = trimAscii(v);
  if (s.empty()) return false;
  char* endPtr = nullptr;
  long parsed = std::strtol(s.c_str(), &endPtr, 10);
  if (!endPtr || *endPtr != '\0') return false;
  if (parsed < (std::numeric_limits<int>::min)() ||
      parsed > (std::numeric_limits<int>::max)()) {
    return false;
  }
  *out = static_cast<int>(parsed);
  return true;
}

bool parseFloatAscii(std::string_view v, float* out) {
  if (!out) return false;
  std::string s = trimAscii(v);
  if (s.empty()) return false;
  char* endPtr = nullptr;
  const float parsed = std::strtof(s.c_str(), &endPtr);
  if (!endPtr || *endPtr != '\0' || !std::isfinite(parsed)) return false;
  *out = parsed;
  return true;
}

int clampAssAlignment(int alignment) {
  if (alignment < 1 || alignment > 9) return 2;
  return alignment;
}

struct AssStyleInfo {
  int alignment = 2;
  int marginL = 0;
  int marginR = 0;
  int marginV = 0;
  float fontSize = 0.0f;
  float scaleX = 1.0f;
  float scaleY = 1.0f;
  std::string fontName;
  bool bold = false;
  bool italic = false;
  bool underline = false;
  bool hasPrimaryColor = false;
  uint8_t primaryR = 255;
  uint8_t primaryG = 255;
  uint8_t primaryB = 255;
  float primaryAlpha = 1.0f;
  bool hasBackColor = false;
  uint8_t backR = 0;
  uint8_t backG = 0;
  uint8_t backB = 0;
  float backAlpha = 0.55f;
};

struct AssScriptContext {
  int playResX = 0;
  int playResY = 0;
  float baseFontSize = 0.0f;
  std::unordered_map<std::string, AssStyleInfo> styles;
};

struct AssOverrideInfo {
  int alignment = -1;
  bool hasPosition = false;
  float posX = 0.0f;
  float posY = 0.0f;
  bool hasMove = false;
  float moveStartX = 0.0f;
  float moveStartY = 0.0f;
  float moveEndX = 0.0f;
  float moveEndY = 0.0f;
  int moveStartMs = 0;
  int moveEndMs = 0;
  float fontSize = 0.0f;
  float scaleX = 0.0f;
  float scaleY = 0.0f;
  bool hasFontName = false;
  std::string fontName;
  bool hasBold = false;
  bool bold = false;
  bool hasItalic = false;
  bool italic = false;
  bool hasUnderline = false;
  bool underline = false;
  bool hasPrimaryColor = false;
  uint8_t primaryR = 255;
  uint8_t primaryG = 255;
  uint8_t primaryB = 255;
  float primaryAlpha = 1.0f;
  bool hasBackColor = false;
  uint8_t backR = 0;
  uint8_t backG = 0;
  uint8_t backB = 0;
  float backAlpha = 0.55f;
};

SubtitleCue makePlainCue(int64_t startUs, int64_t endUs, std::string text,
                         std::string rawText) {
  SubtitleCue cue;
  cue.startUs = startUs;
  cue.endUs = endUs;
  cue.text = std::move(text);
  cue.rawText = std::move(rawText);
  return cue;
}

bool parseAssColorValue(std::string_view token, uint8_t* outR, uint8_t* outG,
                        uint8_t* outB, float* outAlpha) {
  if (!outR || !outG || !outB || !outAlpha) return false;
  token = trimAsciiView(token);
  if (token.empty()) return false;

  size_t start = 0;
  size_t marker = token.find("&H");
  if (marker == std::string_view::npos) marker = token.find("&h");
  if (marker != std::string_view::npos) {
    start = marker + 2;
  } else if (token[0] == 'H' || token[0] == 'h') {
    start = 1;
  }

  uint32_t value = 0;
  bool sawHex = false;
  for (size_t i = start; i < token.size(); ++i) {
    char ch = token[i];
    uint32_t digit = 0;
    if (ch >= '0' && ch <= '9') {
      digit = static_cast<uint32_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      digit = static_cast<uint32_t>(10 + (ch - 'a'));
    } else if (ch >= 'A' && ch <= 'F') {
      digit = static_cast<uint32_t>(10 + (ch - 'A'));
    } else {
      if (sawHex) break;
      continue;
    }
    sawHex = true;
    value = (value << 4) | digit;
  }
  if (!sawHex) return false;

  *outB = static_cast<uint8_t>((value >> 16) & 0xFFu);
  *outG = static_cast<uint8_t>((value >> 8) & 0xFFu);
  *outR = static_cast<uint8_t>(value & 0xFFu);
  const uint8_t assAlpha = static_cast<uint8_t>((value >> 24) & 0xFFu);
  *outAlpha =
      std::clamp(static_cast<float>(255u - assAlpha) / 255.0f, 0.0f, 1.0f);
  return true;
}

void parseAssOverrideBlock(std::string_view block, AssOverrideInfo* out) {
  if (!out || block.empty()) return;
  size_t i = 0;
  while (i < block.size()) {
    if (block[i] != '\\') {
      ++i;
      continue;
    }
    ++i;
    if (i >= block.size()) break;

    if (startsWithAsciiNoCase(block, i, "an")) {
      i += 2;
      size_t start = i;
      while (i < block.size() &&
             std::isdigit(static_cast<unsigned char>(block[i])) != 0) {
        ++i;
      }
      int value = 0;
      if (parseNonNegativeInt(block.substr(start, i - start), &value)) {
        out->alignment = clampAssAlignment(value);
      }
      continue;
    }

    auto parseColorToken = [&](size_t tagLen, bool backColor) {
      i += tagLen;
      size_t valueStart = i;
      while (i < block.size() && block[i] != '\\') ++i;
      uint8_t r = 255;
      uint8_t g = 255;
      uint8_t b = 255;
      float alpha = 1.0f;
      if (parseAssColorValue(block.substr(valueStart, i - valueStart),
                             &r, &g, &b, &alpha)) {
        if (backColor) {
          out->hasBackColor = true;
          out->backR = r;
          out->backG = g;
          out->backB = b;
          out->backAlpha = alpha;
        } else {
          out->hasPrimaryColor = true;
          out->primaryR = r;
          out->primaryG = g;
          out->primaryB = b;
          out->primaryAlpha = alpha;
        }
      }
    };

    if (i + 2 <= block.size() && block[i] == '1' &&
        (block[i + 1] == 'c' || block[i + 1] == 'C')) {
      parseColorToken(2, false);
      continue;
    }
    if (i + 2 <= block.size() && block[i] == '4' &&
        (block[i + 1] == 'c' || block[i + 1] == 'C')) {
      parseColorToken(2, true);
      continue;
    }
    if ((block[i] == 'c' || block[i] == 'C') &&
        !(i + 1 < block.size() &&
          (block[i + 1] == 'l' || block[i + 1] == 'L'))) {
      parseColorToken(1, false);
      continue;
    }

    if (startsWithAsciiNoCase(block, i, "pos")) {
      i += 3;
      if (i >= block.size() || block[i] != '(') continue;
      ++i;
      size_t end = block.find(')', i);
      if (end == std::string_view::npos) continue;
      std::string args = std::string(block.substr(i, end - i));
      size_t comma = args.find(',');
      if (comma != std::string::npos) {
        float x = 0.0f;
        float y = 0.0f;
        if (parseFloatAscii(args.substr(0, comma), &x) &&
            parseFloatAscii(args.substr(comma + 1), &y)) {
          out->hasPosition = true;
          out->posX = x;
          out->posY = y;
        }
      }
      i = end + 1;
      continue;
    }

    if (startsWithAsciiNoCase(block, i, "move")) {
      i += 4;
      if (i >= block.size() || block[i] != '(') continue;
      ++i;
      size_t end = block.find(')', i);
      if (end == std::string_view::npos) continue;
      std::string args = std::string(block.substr(i, end - i));
      std::vector<std::string> parts;
      size_t partStart = 0;
      while (partStart <= args.size()) {
        size_t comma = args.find(',', partStart);
        parts.push_back(trimAscii(
            comma == std::string::npos
                ? std::string_view(args).substr(partStart)
                : std::string_view(args).substr(partStart,
                                                comma - partStart)));
        if (comma == std::string::npos) break;
        partStart = comma + 1;
      }
      if (parts.size() >= 4) {
        float x1 = 0.0f;
        float y1 = 0.0f;
        float x2 = 0.0f;
        float y2 = 0.0f;
        if (parseFloatAscii(parts[0], &x1) &&
            parseFloatAscii(parts[1], &y1) &&
            parseFloatAscii(parts[2], &x2) &&
            parseFloatAscii(parts[3], &y2)) {
          out->hasPosition = true;
          out->posX = x1;
          out->posY = y1;
          out->hasMove = true;
          out->moveStartX = x1;
          out->moveStartY = y1;
          out->moveEndX = x2;
          out->moveEndY = y2;
          int t1 = 0;
          int t2 = 0;
          if (parts.size() >= 6 && parseNonNegativeInt(parts[4], &t1) &&
              parseNonNegativeInt(parts[5], &t2) && t2 > t1) {
            out->moveStartMs = t1;
            out->moveEndMs = t2;
          } else {
            out->moveStartMs = 0;
            out->moveEndMs = 0;
          }
        }
      }
      i = end + 1;
      continue;
    }

    if (startsWithAsciiNoCase(block, i, "fscx")) {
      i += 4;
      size_t start = i;
      if (i < block.size() && (block[i] == '+' || block[i] == '-')) ++i;
      while (i < block.size() &&
             (std::isdigit(static_cast<unsigned char>(block[i])) != 0 ||
              block[i] == '.')) {
        ++i;
      }
      float value = 0.0f;
      if (parseFloatAscii(block.substr(start, i - start), &value) &&
          value > 0.0f) {
        out->scaleX = value * 0.01f;
      }
      continue;
    }

    if (startsWithAsciiNoCase(block, i, "fscy")) {
      i += 4;
      size_t start = i;
      if (i < block.size() && (block[i] == '+' || block[i] == '-')) ++i;
      while (i < block.size() &&
             (std::isdigit(static_cast<unsigned char>(block[i])) != 0 ||
              block[i] == '.')) {
        ++i;
      }
      float value = 0.0f;
      if (parseFloatAscii(block.substr(start, i - start), &value) &&
          value > 0.0f) {
        out->scaleY = value * 0.01f;
      }
      continue;
    }

    if (startsWithAsciiNoCase(block, i, "fs")) {
      i += 2;
      size_t start = i;
      if (i < block.size() && (block[i] == '+' || block[i] == '-')) ++i;
      while (i < block.size() &&
             (std::isdigit(static_cast<unsigned char>(block[i])) != 0 ||
              block[i] == '.')) {
        ++i;
      }
      float value = 0.0f;
      if (parseFloatAscii(block.substr(start, i - start), &value) &&
          value > 0.0f) {
        out->fontSize = value;
      }
      continue;
    }

    if (startsWithAsciiNoCase(block, i, "fn")) {
      i += 2;
      size_t start = i;
      while (i < block.size() && block[i] != '\\') {
        ++i;
      }
      std::string value = trimAscii(block.substr(start, i - start));
      if (!value.empty()) {
        out->hasFontName = true;
        out->fontName = std::move(value);
      }
      continue;
    }

    if (i < block.size() && block[i] == 'b') {
      ++i;
      size_t start = i;
      if (i < block.size() && (block[i] == '+' || block[i] == '-')) ++i;
      while (i < block.size() &&
             std::isdigit(static_cast<unsigned char>(block[i])) != 0) {
        ++i;
      }
      int value = 0;
      if (parseSignedInt(block.substr(start, i - start), &value)) {
        out->hasBold = true;
        out->bold = value != 0;
      }
      continue;
    }

    if (i < block.size() && block[i] == 'i') {
      ++i;
      size_t start = i;
      if (i < block.size() && (block[i] == '+' || block[i] == '-')) ++i;
      while (i < block.size() &&
             std::isdigit(static_cast<unsigned char>(block[i])) != 0) {
        ++i;
      }
      int value = 0;
      if (parseSignedInt(block.substr(start, i - start), &value)) {
        out->hasItalic = true;
        out->italic = value != 0;
      }
      continue;
    }

    if (i < block.size() && block[i] == 'u') {
      ++i;
      size_t start = i;
      if (i < block.size() && (block[i] == '+' || block[i] == '-')) ++i;
      while (i < block.size() &&
             std::isdigit(static_cast<unsigned char>(block[i])) != 0) {
        ++i;
      }
      int value = 0;
      if (parseSignedInt(block.substr(start, i - start), &value)) {
        out->hasUnderline = true;
        out->underline = value != 0;
      }
      continue;
    }
  }
}

void parseAssOverridesFromText(const std::string& rawText, AssOverrideInfo* out) {
  if (!out || rawText.empty()) return;
  size_t start = 0;
  while (start < rawText.size()) {
    size_t open = rawText.find('{', start);
    if (open == std::string::npos) break;
    size_t close = rawText.find('}', open + 1);
    if (close == std::string::npos) break;
    parseAssOverrideBlock(
        std::string_view(rawText.data() + open + 1, close - open - 1), out);
    start = close + 1;
  }
}

void replaceAll(std::string* text, const std::string& from,
                const std::string& to) {
  if (!text || from.empty()) return;
  size_t pos = 0;
  while ((pos = text->find(from, pos)) != std::string::npos) {
    text->replace(pos, from.size(), to);
    pos += to.size();
  }
}

bool parseAssAlphaValue(std::string_view token, int* outAlpha) {
  if (!outAlpha) return false;
  token = trimAsciiView(token);
  if (token.empty()) return false;
  size_t start = 0;
  size_t marker = token.find("&H");
  if (marker == std::string_view::npos) marker = token.find("&h");
  if (marker != std::string_view::npos) {
    start = marker + 2;
  } else if (token.size() >= 1 && (token[0] == 'H' || token[0] == 'h')) {
    start = 1;
  }

  unsigned value = 0;
  bool sawHex = false;
  for (size_t i = start; i < token.size(); ++i) {
    char ch = token[i];
    unsigned digit = 0;
    if (ch >= '0' && ch <= '9') {
      digit = static_cast<unsigned>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      digit = static_cast<unsigned>(10 + (ch - 'a'));
    } else if (ch >= 'A' && ch <= 'F') {
      digit = static_cast<unsigned>(10 + (ch - 'A'));
    } else {
      if (sawHex) break;
      continue;
    }
    sawHex = true;
    value = (value << 4) | digit;
  }
  if (!sawHex) return false;
  *outAlpha = static_cast<int>(value & 0xFFu);
  return true;
}

void parseAssTextOverrides(std::string_view block, int* inOutPrimaryAlpha,
                           int* inOutDrawingMode) {
  if (!inOutPrimaryAlpha || !inOutDrawingMode || block.empty()) return;
  size_t i = 0;
  while (i < block.size()) {
    if (block[i] != '\\') {
      ++i;
      continue;
    }
    ++i;
    if (i >= block.size()) break;

    if (block[i] == 'r' || block[i] == 'R') {
      *inOutPrimaryAlpha = 0;
      *inOutDrawingMode = 0;
      ++i;
      while (i < block.size() && block[i] != '\\') ++i;
      continue;
    }

    if ((block[i] == 'p' || block[i] == 'P') &&
        !(i + 1 < block.size() &&
          (block[i + 1] == 'o' || block[i + 1] == 'O' || block[i + 1] == 'b' ||
           block[i + 1] == 'B'))) {
      ++i;
      size_t valueStart = i;
      if (i < block.size() && (block[i] == '+' || block[i] == '-')) ++i;
      while (i < block.size() &&
             std::isdigit(static_cast<unsigned char>(block[i])) != 0) {
        ++i;
      }
      int drawingMode = 0;
      if (parseSignedInt(block.substr(valueStart, i - valueStart), &drawingMode)) {
        *inOutDrawingMode = (std::max)(0, drawingMode);
      }
      continue;
    }

    auto parseAlphaToken = [&](size_t tagLen) {
      i += tagLen;
      size_t valueStart = i;
      while (i < block.size() && block[i] != '\\') ++i;
      int parsedAlpha = 0;
      if (parseAssAlphaValue(block.substr(valueStart, i - valueStart),
                             &parsedAlpha)) {
        *inOutPrimaryAlpha = parsedAlpha;
      }
    };

    if (i + 5 <= block.size() &&
        (block[i] == 'a' || block[i] == 'A') &&
        (block[i + 1] == 'l' || block[i + 1] == 'L') &&
        (block[i + 2] == 'p' || block[i + 2] == 'P') &&
        (block[i + 3] == 'h' || block[i + 3] == 'H') &&
        (block[i + 4] == 'a' || block[i + 4] == 'A')) {
      parseAlphaToken(5);
      continue;
    }
    if (i + 2 <= block.size() && block[i] == '1' &&
        (block[i + 1] == 'a' || block[i + 1] == 'A')) {
      parseAlphaToken(2);
      continue;
    }
  }
}

struct AssInlineTextStyle {
  bool hasPrimaryColor = false;
  uint8_t primaryR = 255;
  uint8_t primaryG = 255;
  uint8_t primaryB = 255;
  float primaryAlpha = 1.0f;
  bool hasBackColor = false;
  uint8_t backR = 0;
  uint8_t backG = 0;
  uint8_t backB = 0;
  float backAlpha = 0.55f;
  int drawingMode = 0;
};

AssInlineTextStyle assInlineTextStyleFromStyle(const AssStyleInfo& style) {
  AssInlineTextStyle out;
  out.hasPrimaryColor = style.hasPrimaryColor;
  out.primaryR = style.primaryR;
  out.primaryG = style.primaryG;
  out.primaryB = style.primaryB;
  out.primaryAlpha = style.primaryAlpha;
  out.hasBackColor = style.hasBackColor;
  out.backR = style.backR;
  out.backG = style.backG;
  out.backB = style.backB;
  out.backAlpha = style.backAlpha;
  return out;
}

bool sameSubtitleTextRunStyle(const SubtitleTextRun& run,
                              const AssInlineTextStyle& style) {
  return run.hasPrimaryColor == style.hasPrimaryColor &&
         run.primaryR == style.primaryR && run.primaryG == style.primaryG &&
         run.primaryB == style.primaryB &&
         std::abs(run.primaryAlpha - style.primaryAlpha) < 0.001f &&
         run.hasBackColor == style.hasBackColor && run.backR == style.backR &&
         run.backG == style.backG && run.backB == style.backB &&
         std::abs(run.backAlpha - style.backAlpha) < 0.001f;
}

void appendAssStyledText(std::string_view text,
                         const AssInlineTextStyle& style,
                         std::string* outText,
                         std::vector<SubtitleTextRun>* outRuns) {
  if (text.empty() || !outText || !outRuns || style.drawingMode != 0 ||
      style.primaryAlpha <= 0.001f) {
    return;
  }
  outText->append(text);
  if (!outRuns->empty() && sameSubtitleTextRunStyle(outRuns->back(), style)) {
    outRuns->back().text.append(text);
    return;
  }
  SubtitleTextRun run;
  run.text.assign(text);
  run.hasPrimaryColor = style.hasPrimaryColor;
  run.primaryR = style.primaryR;
  run.primaryG = style.primaryG;
  run.primaryB = style.primaryB;
  run.primaryAlpha = style.primaryAlpha;
  run.hasBackColor = style.hasBackColor;
  run.backR = style.backR;
  run.backG = style.backG;
  run.backB = style.backB;
  run.backAlpha = style.backAlpha;
  outRuns->push_back(std::move(run));
}

void parseAssInlineTextOverrideBlock(
    std::string_view block, const AssStyleInfo& baseStyle,
    const std::unordered_map<std::string, AssStyleInfo>* styles,
    AssInlineTextStyle* style) {
  if (!style || block.empty()) return;
  size_t i = 0;
  while (i < block.size()) {
    if (block[i] != '\\') {
      ++i;
      continue;
    }
    ++i;
    if (i >= block.size()) break;

    if (block[i] == 'r' || block[i] == 'R') {
      ++i;
      size_t styleStart = i;
      while (i < block.size() && block[i] != '\\') ++i;
      const std::string resetStyleName =
          toLowerAscii(trimAscii(block.substr(styleStart, i - styleStart)));
      const AssStyleInfo* resetStyle = &baseStyle;
      if (!resetStyleName.empty() && styles) {
        auto it = styles->find(resetStyleName);
        if (it != styles->end()) resetStyle = &it->second;
      }
      *style = assInlineTextStyleFromStyle(*resetStyle);
      continue;
    }

    if ((block[i] == 'p' || block[i] == 'P') &&
        !(i + 1 < block.size() &&
          (block[i + 1] == 'o' || block[i + 1] == 'O' || block[i + 1] == 'b' ||
           block[i + 1] == 'B'))) {
      ++i;
      size_t valueStart = i;
      if (i < block.size() && (block[i] == '+' || block[i] == '-')) ++i;
      while (i < block.size() &&
             std::isdigit(static_cast<unsigned char>(block[i])) != 0) {
        ++i;
      }
      int drawingMode = 0;
      if (parseSignedInt(block.substr(valueStart, i - valueStart),
                         &drawingMode)) {
        style->drawingMode = std::max(0, drawingMode);
      }
      continue;
    }

    auto parseColorToken = [&](size_t tagLen, bool backColor) {
      i += tagLen;
      size_t valueStart = i;
      while (i < block.size() && block[i] != '\\') ++i;
      uint8_t r = 255;
      uint8_t g = 255;
      uint8_t b = 255;
      float alpha = 1.0f;
      if (parseAssColorValue(block.substr(valueStart, i - valueStart),
                             &r, &g, &b, &alpha)) {
        if (backColor) {
          style->hasBackColor = true;
          style->backR = r;
          style->backG = g;
          style->backB = b;
          style->backAlpha = alpha;
        } else {
          style->hasPrimaryColor = true;
          style->primaryR = r;
          style->primaryG = g;
          style->primaryB = b;
          style->primaryAlpha = alpha;
        }
      }
    };

    if (i + 2 <= block.size() && block[i] == '1' &&
        (block[i + 1] == 'c' || block[i + 1] == 'C')) {
      parseColorToken(2, false);
      continue;
    }
    if (i + 2 <= block.size() && block[i] == '4' &&
        (block[i + 1] == 'c' || block[i + 1] == 'C')) {
      parseColorToken(2, true);
      continue;
    }
    if ((block[i] == 'c' || block[i] == 'C') &&
        !(i + 1 < block.size() &&
          (block[i + 1] == 'l' || block[i + 1] == 'L'))) {
      parseColorToken(1, false);
      continue;
    }

    auto parseAlphaToken = [&](size_t tagLen, bool primary, bool back) {
      i += tagLen;
      size_t valueStart = i;
      while (i < block.size() && block[i] != '\\') ++i;
      int parsedAlpha = 0;
      if (parseAssAlphaValue(block.substr(valueStart, i - valueStart),
                             &parsedAlpha)) {
        const float alpha = std::clamp(
            static_cast<float>(255 - parsedAlpha) / 255.0f, 0.0f, 1.0f);
        if (primary) style->primaryAlpha = alpha;
        if (back) style->backAlpha = alpha;
      }
    };

    if (i + 5 <= block.size() &&
        (block[i] == 'a' || block[i] == 'A') &&
        (block[i + 1] == 'l' || block[i + 1] == 'L') &&
        (block[i + 2] == 'p' || block[i + 2] == 'P') &&
        (block[i + 3] == 'h' || block[i + 3] == 'H') &&
        (block[i + 4] == 'a' || block[i + 4] == 'A')) {
      parseAlphaToken(5, true, true);
      continue;
    }
    if (i + 2 <= block.size() && block[i] == '1' &&
        (block[i + 1] == 'a' || block[i + 1] == 'A')) {
      parseAlphaToken(2, true, false);
      continue;
    }
    if (i + 2 <= block.size() && block[i] == '4' &&
        (block[i + 1] == 'a' || block[i + 1] == 'A')) {
      parseAlphaToken(2, false, true);
      continue;
    }

    while (i < block.size() && block[i] != '\\') ++i;
  }
}

bool parseAssStyledTextRuns(
    const std::string& rawText, const AssStyleInfo& baseStyle,
    const std::unordered_map<std::string, AssStyleInfo>* styles,
    std::string* outText, std::vector<SubtitleTextRun>* outRuns) {
  if (!outText || !outRuns) return false;
  outText->clear();
  outRuns->clear();
  if (rawText.empty()) return false;

  AssInlineTextStyle style = assInlineTextStyleFromStyle(baseStyle);
  size_t pos = 0;
  while (pos < rawText.size()) {
    if (rawText[pos] == '{') {
      size_t close = rawText.find('}', pos + 1);
      if (close == std::string::npos) {
        ++pos;
        continue;
      }
      parseAssInlineTextOverrideBlock(
          std::string_view(rawText.data() + pos + 1, close - pos - 1),
          baseStyle, styles, &style);
      pos = close + 1;
      continue;
    }

    if (rawText[pos] == '\\' && pos + 1 < rawText.size()) {
      const char tag = rawText[pos + 1];
      if (tag == 'N' || tag == 'n') {
        appendAssStyledText("\n", style, outText, outRuns);
        pos += 2;
        continue;
      }
      if (tag == 'h') {
        appendAssStyledText(" ", style, outText, outRuns);
        pos += 2;
        continue;
      }
    }

    const size_t literalStart = pos;
    while (pos < rawText.size() && rawText[pos] != '{' &&
           !(rawText[pos] == '\\' && pos + 1 < rawText.size() &&
             (rawText[pos + 1] == 'N' || rawText[pos + 1] == 'n' ||
              rawText[pos + 1] == 'h'))) {
      ++pos;
    }
    appendAssStyledText(
        std::string_view(rawText.data() + literalStart, pos - literalStart),
        style, outText, outRuns);
  }

  *outText = trimAscii(*outText);
  return !outText->empty();
}

std::string stripSubtitleMarkup(const std::string& in) {
  std::string noHtml;
  noHtml.reserve(in.size());
  bool inTag = false;
  for (char ch : in) {
    if (ch == '<') {
      inTag = true;
      continue;
    }
    if (inTag) {
      if (ch == '>') inTag = false;
      continue;
    }
    noHtml.push_back(ch);
  }

  std::string noAss;
  noAss.reserve(noHtml.size());
  int primaryAlpha = 0;  // ASS alpha: 0 = opaque, 255 = transparent
  int drawingMode = 0;   // ASS \p vector drawing mode: 0 = text.
  size_t pos = 0;
  while (pos < noHtml.size()) {
    if (noHtml[pos] == '{') {
      size_t close = noHtml.find('}', pos + 1);
      if (close == std::string::npos) {
        ++pos;
        continue;
      }
      parseAssTextOverrides(
          std::string_view(noHtml.data() + pos + 1, close - pos - 1),
          &primaryAlpha, &drawingMode);
      pos = close + 1;
      continue;
    }
    if (primaryAlpha < 255 && drawingMode == 0) {
      noAss.push_back(noHtml[pos]);
    }
    ++pos;
  }

  replaceAll(&noAss, "&nbsp;", " ");
  replaceAll(&noAss, "&amp;", "&");
  replaceAll(&noAss, "&lt;", "<");
  replaceAll(&noAss, "&gt;", ">");
  return trimAscii(noAss);
}

std::vector<std::string> splitLinesNormalized(const std::string& raw) {
  std::vector<std::string> lines;
  std::string text = raw;
  if (text.size() >= 3 &&
      static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB &&
      static_cast<unsigned char>(text[2]) == 0xBF) {
    text.erase(0, 3);
  }

  std::string normalized;
  normalized.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    char ch = text[i];
    if (ch == '\r') {
      if (i + 1 < text.size() && text[i + 1] == '\n') {
        continue;
      }
      normalized.push_back('\n');
      continue;
    }
    normalized.push_back(ch);
  }

  size_t start = 0;
  while (start <= normalized.size()) {
    size_t end = normalized.find('\n', start);
    if (end == std::string::npos) {
      lines.push_back(normalized.substr(start));
      break;
    }
    lines.push_back(normalized.substr(start, end - start));
    start = end + 1;
    if (start == normalized.size()) {
      lines.emplace_back();
      break;
    }
  }
  return lines;
}

void appendUtf8Codepoint(uint32_t cp, std::string* out) {
  if (!out) return;
  if (cp <= 0x7Fu) {
    out->push_back(static_cast<char>(cp));
    return;
  }
  if (cp <= 0x7FFu) {
    out->push_back(static_cast<char>(0xC0u | ((cp >> 6) & 0x1Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    return;
  }
  if (cp <= 0xFFFFu) {
    out->push_back(static_cast<char>(0xE0u | ((cp >> 12) & 0x0Fu)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
    return;
  }
  if (cp <= 0x10FFFFu) {
    out->push_back(static_cast<char>(0xF0u | ((cp >> 18) & 0x07u)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out->push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  }
}

bool decodeUtf16ToUtf8(const std::string& rawBytes, bool littleEndian,
                       size_t startOffset, std::string* outUtf8) {
  if (!outUtf8) return false;
  outUtf8->clear();
  if (startOffset > rawBytes.size()) return false;
  if (((rawBytes.size() - startOffset) & 1u) != 0u) return false;

  auto readUnit = [&](size_t i) -> uint16_t {
    uint16_t b0 = static_cast<unsigned char>(rawBytes[i]);
    uint16_t b1 = static_cast<unsigned char>(rawBytes[i + 1]);
    return littleEndian ? static_cast<uint16_t>(b0 | (b1 << 8))
                        : static_cast<uint16_t>(b1 | (b0 << 8));
  };

  size_t i = startOffset;
  while (i + 1 < rawBytes.size()) {
    uint16_t unit = readUnit(i);
    i += 2;
    if (unit == 0xFEFFu) continue;

    uint32_t codepoint = unit;
    if (unit >= 0xD800u && unit <= 0xDBFFu) {
      if (i + 1 >= rawBytes.size()) return false;
      uint16_t low = readUnit(i);
      if (low < 0xDC00u || low > 0xDFFFu) return false;
      i += 2;
      codepoint = 0x10000u +
                  ((static_cast<uint32_t>(unit) - 0xD800u) << 10) +
                  (static_cast<uint32_t>(low) - 0xDC00u);
    } else if (unit >= 0xDC00u && unit <= 0xDFFFu) {
      return false;
    }
    appendUtf8Codepoint(codepoint, outUtf8);
  }
  return !outUtf8->empty();
}

bool looksLikeUtf16WithoutBom(const std::string& rawBytes,
                              bool* outLittleEndian) {
  if (!outLittleEndian) return false;
  if (rawBytes.size() < 8) return false;
  const size_t sampleSize = std::min<size_t>(rawBytes.size(), 4096u);
  size_t evenCount = 0;
  size_t oddCount = 0;
  size_t evenZero = 0;
  size_t oddZero = 0;
  for (size_t i = 0; i < sampleSize; ++i) {
    if ((i & 1u) == 0u) {
      ++evenCount;
      if (rawBytes[i] == '\0') ++evenZero;
    } else {
      ++oddCount;
      if (rawBytes[i] == '\0') ++oddZero;
    }
  }
  if (evenCount == 0 || oddCount == 0) return false;

  const double evenRatio = static_cast<double>(evenZero) / evenCount;
  const double oddRatio = static_cast<double>(oddZero) / oddCount;
  if (oddRatio > 0.35 && evenRatio < 0.05) {
    *outLittleEndian = true;
    return true;
  }
  if (evenRatio > 0.35 && oddRatio < 0.05) {
    *outLittleEndian = false;
    return true;
  }
  return false;
}

std::string decodeSubtitleText(const std::string& rawBytes) {
  if (rawBytes.empty()) return {};
  std::string decoded;

  if (rawBytes.size() >= 2) {
    unsigned char b0 = static_cast<unsigned char>(rawBytes[0]);
    unsigned char b1 = static_cast<unsigned char>(rawBytes[1]);
    if (b0 == 0xFFu && b1 == 0xFEu) {
      if (decodeUtf16ToUtf8(rawBytes, true, 2, &decoded)) return decoded;
    } else if (b0 == 0xFEu && b1 == 0xFFu) {
      if (decodeUtf16ToUtf8(rawBytes, false, 2, &decoded)) return decoded;
    }
  }

  bool littleEndian = true;
  if (looksLikeUtf16WithoutBom(rawBytes, &littleEndian) &&
      decodeUtf16ToUtf8(rawBytes, littleEndian, 0, &decoded)) {
    return decoded;
  }
  return rawBytes;
}

bool parsePositiveDouble(std::string_view text, double* outValue) {
  if (!outValue) return false;
  std::string trimmed = trimAscii(text);
  if (trimmed.empty()) return false;
  char* end = nullptr;
  double value = std::strtod(trimmed.c_str(), &end);
  if (!end || *end != '\0' || !std::isfinite(value) || value <= 0.0) {
    return false;
  }
  *outValue = value;
  return true;
}

bool parseTimecodeUs(std::string_view token, int64_t* outUs) {
  if (!outUs) return false;
  token = trimAsciiView(token);
  if (token.empty()) return false;

  std::string normalized(token);
  for (char& ch : normalized) {
    if (ch == ',') ch = '.';
  }

  std::array<std::string_view, 3> parts{};
  int partCount = 0;
  std::string_view view(normalized);
  size_t begin = 0;
  while (begin <= view.size()) {
    size_t sep = view.find(':', begin);
    std::string_view piece =
        (sep == std::string::npos) ? view.substr(begin)
                                   : view.substr(begin, sep - begin);
    if (piece.empty() || partCount >= 3) return false;
    parts[partCount++] = piece;
    if (sep == std::string::npos) break;
    begin = sep + 1;
  }
  if (partCount != 2 && partCount != 3) return false;

  int hours = 0;
  int minutes = 0;
  std::string_view secPart;
  if (partCount == 3) {
    if (!parseNonNegativeInt(parts[0], &hours)) return false;
    if (!parseNonNegativeInt(parts[1], &minutes)) return false;
    secPart = parts[2];
  } else {
    if (!parseNonNegativeInt(parts[0], &minutes)) return false;
    secPart = parts[1];
  }

  size_t dot = secPart.find('.');
  std::string_view secDigits =
      (dot == std::string::npos) ? secPart : secPart.substr(0, dot);
  int seconds = 0;
  if (!parseNonNegativeInt(secDigits, &seconds)) return false;
  int millis = 0;
  if (dot != std::string::npos) {
    std::string_view frac = secPart.substr(dot + 1);
    if (frac.empty()) return false;
    int digits = 0;
    for (char ch : frac) {
      if (std::isdigit(static_cast<unsigned char>(ch)) == 0) return false;
      if (digits < 3) {
        millis = millis * 10 + (ch - '0');
      }
      ++digits;
    }
    while (digits < 3) {
      millis *= 10;
      ++digits;
    }
  }

  if (minutes >= 60 || seconds >= 60) return false;

  *outUs = (static_cast<int64_t>(hours) * 3600LL +
            static_cast<int64_t>(minutes) * 60LL +
            static_cast<int64_t>(seconds)) *
               1000000LL +
           static_cast<int64_t>(millis) * 1000LL;
  return true;
}

bool parseCueWindow(std::string_view line, int64_t* outStartUs,
                    int64_t* outEndUs) {
  if (!outStartUs || !outEndUs) return false;
  size_t arrow = line.find("-->");
  if (arrow == std::string::npos) return false;
  std::string_view lhs = trimAsciiView(line.substr(0, arrow));
  std::string_view rhs = trimAsciiView(line.substr(arrow + 3));
  if (lhs.empty() || rhs.empty()) return false;
  size_t rhsSpace = rhs.find_first_of(" \t");
  if (rhsSpace != std::string::npos) {
    rhs = rhs.substr(0, rhsSpace);
  }
  int64_t startUs = 0;
  int64_t endUs = 0;
  if (!parseTimecodeUs(lhs, &startUs)) return false;
  if (!parseTimecodeUs(rhs, &endUs)) return false;
  if (endUs <= startUs) {
    endUs = startUs + 100000;
  }
  *outStartUs = startUs;
  *outEndUs = endUs;
  return true;
}

std::string joinCueText(const std::vector<std::string>& lines) {
  std::string out;
  for (const std::string& line : lines) {
    std::string cleaned = stripSubtitleMarkup(line);
    if (cleaned.empty()) continue;
    if (!out.empty()) out.push_back('\n');
    out += cleaned;
  }
  return out;
}

bool parseSrtCues(const std::string& raw, std::vector<SubtitleCue>* outCues) {
  if (!outCues) return false;
  std::vector<std::string> lines = splitLinesNormalized(raw);
  if (lines.empty()) return false;

  size_t i = 0;
  while (i < lines.size()) {
    while (i < lines.size() && trimAsciiView(lines[i]).empty()) {
      ++i;
    }
    if (i >= lines.size()) break;

    std::string_view timingLine = trimAsciiView(lines[i]);
    if (timingLine.find("-->") == std::string::npos) {
      if (isDigitsOnly(timingLine) && i + 1 < lines.size()) {
        ++i;
        timingLine = trimAsciiView(lines[i]);
      }
    }
    if (timingLine.find("-->") == std::string::npos) {
      ++i;
      continue;
    }

    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!parseCueWindow(timingLine, &startUs, &endUs)) {
      ++i;
      continue;
    }
    ++i;

    std::vector<std::string> cueLines;
    while (i < lines.size() && !trimAsciiView(lines[i]).empty()) {
      cueLines.push_back(lines[i]);
      ++i;
    }

    std::string text = joinCueText(cueLines);
    if (!text.empty()) {
      std::string rawText = text;
      outCues->push_back(
          makePlainCue(startUs, endUs, std::move(text), std::move(rawText)));
    }
  }
  return !outCues->empty();
}

bool parseVttCues(const std::string& raw, std::vector<SubtitleCue>* outCues) {
  if (!outCues) return false;
  std::vector<std::string> lines = splitLinesNormalized(raw);
  if (lines.empty()) return false;

  size_t i = 0;
  if (!lines.empty()) {
    std::string header = toLowerAscii(trimAscii(lines[0]));
    if (header.rfind("webvtt", 0) == 0) {
      i = 1;
    }
  }

  while (i < lines.size()) {
    std::string_view line = trimAsciiView(lines[i]);
    if (line.empty()) {
      ++i;
      continue;
    }
    if (line.rfind("NOTE", 0) == 0 || line.rfind("note", 0) == 0) {
      ++i;
      while (i < lines.size() && !trimAsciiView(lines[i]).empty()) {
        ++i;
      }
      continue;
    }

    std::string_view timingLine = line;
    if (timingLine.find("-->") == std::string::npos) {
      ++i;
      if (i >= lines.size()) break;
      timingLine = trimAsciiView(lines[i]);
    }
    if (timingLine.find("-->") == std::string::npos) {
      ++i;
      continue;
    }

    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!parseCueWindow(timingLine, &startUs, &endUs)) {
      ++i;
      continue;
    }
    ++i;

    std::vector<std::string> cueLines;
    while (i < lines.size() && !trimAsciiView(lines[i]).empty()) {
      cueLines.push_back(lines[i]);
      ++i;
    }
    std::string text = joinCueText(cueLines);
    if (!text.empty()) {
      std::string rawText = text;
      outCues->push_back(
          makePlainCue(startUs, endUs, std::move(text), std::move(rawText)));
    }
  }
  return !outCues->empty();
}

bool parseSbvCues(const std::string& raw, std::vector<SubtitleCue>* outCues) {
  if (!outCues) return false;
  std::vector<std::string> lines = splitLinesNormalized(raw);
  if (lines.empty()) return false;

  size_t i = 0;
  while (i < lines.size()) {
    while (i < lines.size() && trimAsciiView(lines[i]).empty()) {
      ++i;
    }
    if (i >= lines.size()) break;

    std::string_view timingLine = trimAsciiView(lines[i]);
    size_t comma = timingLine.find(',');
    if (comma == std::string::npos) {
      ++i;
      continue;
    }
    std::string_view lhs = trimAsciiView(timingLine.substr(0, comma));
    std::string_view rhs = trimAsciiView(timingLine.substr(comma + 1));
    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!parseTimecodeUs(lhs, &startUs) || !parseTimecodeUs(rhs, &endUs)) {
      ++i;
      continue;
    }
    if (endUs <= startUs) endUs = startUs + 100000;
    ++i;

    std::vector<std::string> cueLines;
    while (i < lines.size() && !trimAsciiView(lines[i]).empty()) {
      cueLines.push_back(lines[i]);
      ++i;
    }
    std::string text = joinCueText(cueLines);
    if (!text.empty()) {
      std::string rawText = text;
      outCues->push_back(
          makePlainCue(startUs, endUs, std::move(text), std::move(rawText)));
    }
  }

  return !outCues->empty();
}

bool parseMicroDvdLine(std::string_view line, int* outStartFrame, int* outEndFrame,
                       std::string* outText) {
  if (!outStartFrame || !outEndFrame || !outText) return false;
  line = trimAsciiView(line);
  if (line.size() < 7 || line.front() != '{') return false;

  auto parseFrameTag = [&](size_t startPos, int* outFrame, size_t* outNextPos) {
    if (!outFrame || !outNextPos || startPos >= line.size() ||
        line[startPos] != '{') {
      return false;
    }
    size_t close = line.find('}', startPos + 1);
    if (close == std::string::npos || close <= startPos + 1) return false;
    std::string_view digits = line.substr(startPos + 1, close - startPos - 1);
    if (!parseNonNegativeInt(digits, outFrame)) return false;
    *outNextPos = close + 1;
    return true;
  };

  size_t pos = 0;
  int startFrame = 0;
  int endFrame = 0;
  if (!parseFrameTag(pos, &startFrame, &pos)) return false;
  if (!parseFrameTag(pos, &endFrame, &pos)) return false;
  if (startFrame < 0 || endFrame < 0) return false;

  *outStartFrame = startFrame;
  *outEndFrame = endFrame;
  *outText = trimAscii(line.substr(pos));
  return true;
}

bool parseMicroDvdCues(const std::string& raw, std::vector<SubtitleCue>* outCues) {
  if (!outCues) return false;
  std::vector<std::string> lines = splitLinesNormalized(raw);
  if (lines.empty()) return false;

  double fps = 25.0;
  bool fpsExplicit = false;
  for (const std::string& rawLine : lines) {
    int startFrame = 0;
    int endFrame = 0;
    std::string text;
    if (!parseMicroDvdLine(rawLine, &startFrame, &endFrame, &text)) {
      continue;
    }

    if ((startFrame == 1 && endFrame == 1) ||
        (startFrame == 0 && endFrame == 0)) {
      double parsedFps = 0.0;
      if (parsePositiveDouble(text, &parsedFps)) {
        fps = parsedFps;
        fpsExplicit = true;
        continue;
      }
      if (!fpsExplicit) continue;
    }

    if (fps <= 0.0 || !std::isfinite(fps)) continue;
    replaceAll(&text, "|", "\n");
    text = stripSubtitleMarkup(text);
    if (text.empty()) continue;

    int64_t startUs = static_cast<int64_t>(
        std::llround((static_cast<double>(startFrame) * 1000000.0) / fps));
    int64_t endUs = static_cast<int64_t>(
        std::llround((static_cast<double>(endFrame) * 1000000.0) / fps));
    if (endUs <= startUs) endUs = startUs + 100000;
    std::string rawText = text;
    outCues->push_back(
        makePlainCue(startUs, endUs, std::move(text), std::move(rawText)));
  }

  return !outCues->empty();
}

bool splitCsvPrefix(std::string_view text, size_t fieldCount,
                    std::vector<std::string>* outFields) {
  if (!outFields) return false;
  outFields->clear();
  if (fieldCount == 0) return false;
  outFields->reserve(fieldCount);
  size_t start = 0;
  for (size_t i = 0; i + 1 < fieldCount; ++i) {
    size_t comma = text.find(',', start);
    if (comma == std::string::npos) return false;
    outFields->push_back(trimAscii(text.substr(start, comma - start)));
    start = comma + 1;
  }
  outFields->push_back(trimAscii(text.substr(start)));
  return outFields->size() == fieldCount;
}

void parseAssFormatFields(std::string_view payload,
                          std::vector<std::string>* outFields) {
  if (!outFields) return;
  outFields->clear();
  size_t start = 0;
  while (start <= payload.size()) {
    const size_t comma = payload.find(',', start);
    outFields->push_back(
        trimAscii(comma == std::string_view::npos
                      ? payload.substr(start)
                      : payload.substr(start, comma - start)));
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
}

int findAssFieldIndex(const std::vector<std::string>& fields,
                      const char* wanted) {
  if (!wanted) return -1;
  const std::string target = toLowerAscii(std::string(wanted));
  for (size_t i = 0; i < fields.size(); ++i) {
    if (toLowerAscii(trimAscii(fields[i])) == target) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

enum class AssSection { Other, ScriptInfo, Styles, Events };

AssSection assSectionFromLine(const std::string& line) {
  if (line.empty() || line.front() != '[' || line.back() != ']') {
    return AssSection::Other;
  }
  const std::string sectionName = toLowerAscii(line);
  if (sectionName == "[script info]") return AssSection::ScriptInfo;
  if (sectionName == "[v4+ styles]" || sectionName == "[v4 styles]") {
    return AssSection::Styles;
  }
  if (sectionName == "[events]") return AssSection::Events;
  return AssSection::Other;
}

struct AssStyleFormatState {
  std::vector<std::string> fields;
  int nameIndex = -1;
  int alignmentIndex = -1;
  int marginLIndex = -1;
  int marginRIndex = -1;
  int marginVIndex = -1;
  int fontSizeIndex = -1;
  int fontNameIndex = -1;
  int boldIndex = -1;
  int italicIndex = -1;
  int underlineIndex = -1;
  int scaleXIndex = -1;
  int scaleYIndex = -1;
  int primaryColourIndex = -1;
  int backColourIndex = -1;
};

void parseAssStyleFormat(std::string_view payload,
                         AssStyleFormatState* state) {
  if (!state) return;
  parseAssFormatFields(payload, &state->fields);
  state->nameIndex = findAssFieldIndex(state->fields, "name");
  state->alignmentIndex = findAssFieldIndex(state->fields, "alignment");
  state->marginLIndex = findAssFieldIndex(state->fields, "marginl");
  state->marginRIndex = findAssFieldIndex(state->fields, "marginr");
  state->marginVIndex = findAssFieldIndex(state->fields, "marginv");
  state->fontSizeIndex = findAssFieldIndex(state->fields, "fontsize");
  state->fontNameIndex = findAssFieldIndex(state->fields, "fontname");
  state->boldIndex = findAssFieldIndex(state->fields, "bold");
  state->italicIndex = findAssFieldIndex(state->fields, "italic");
  state->underlineIndex = findAssFieldIndex(state->fields, "underline");
  state->scaleXIndex = findAssFieldIndex(state->fields, "scalex");
  state->scaleYIndex = findAssFieldIndex(state->fields, "scaley");
  state->primaryColourIndex =
      findAssFieldIndex(state->fields, "primarycolour");
  state->backColourIndex = findAssFieldIndex(state->fields, "backcolour");
}

bool parseAssStyleLine(std::string_view payload,
                       const AssStyleFormatState& format,
                       std::string* outName, AssStyleInfo* outStyle) {
  if (!outName || !outStyle || format.fields.empty() || format.nameIndex < 0) {
    return false;
  }

  std::vector<std::string> fields;
  if (!splitCsvPrefix(payload, format.fields.size(), &fields)) return false;
  if (format.nameIndex >= static_cast<int>(fields.size())) return false;

  std::string styleName =
      trimAscii(fields[static_cast<size_t>(format.nameIndex)]);
  if (styleName.empty()) return false;

  AssStyleInfo style;
  int parsedInt = 0;
  float parsedFloat = 0.0f;
  if (format.alignmentIndex >= 0 &&
      format.alignmentIndex < static_cast<int>(fields.size()) &&
      parseNonNegativeInt(fields[static_cast<size_t>(format.alignmentIndex)],
                          &parsedInt)) {
    style.alignment = clampAssAlignment(parsedInt);
  }
  if (format.marginLIndex >= 0 &&
      format.marginLIndex < static_cast<int>(fields.size()) &&
      parseNonNegativeInt(fields[static_cast<size_t>(format.marginLIndex)],
                          &parsedInt)) {
    style.marginL = parsedInt;
  }
  if (format.marginRIndex >= 0 &&
      format.marginRIndex < static_cast<int>(fields.size()) &&
      parseNonNegativeInt(fields[static_cast<size_t>(format.marginRIndex)],
                          &parsedInt)) {
    style.marginR = parsedInt;
  }
  if (format.marginVIndex >= 0 &&
      format.marginVIndex < static_cast<int>(fields.size()) &&
      parseNonNegativeInt(fields[static_cast<size_t>(format.marginVIndex)],
                          &parsedInt)) {
    style.marginV = parsedInt;
  }
  if (format.fontSizeIndex >= 0 &&
      format.fontSizeIndex < static_cast<int>(fields.size()) &&
      parseFloatAscii(fields[static_cast<size_t>(format.fontSizeIndex)],
                      &parsedFloat) &&
      parsedFloat > 0.0f) {
    style.fontSize = parsedFloat;
  }
  if (format.fontNameIndex >= 0 &&
      format.fontNameIndex < static_cast<int>(fields.size())) {
    style.fontName =
        trimAscii(fields[static_cast<size_t>(format.fontNameIndex)]);
  }
  if (format.boldIndex >= 0 &&
      format.boldIndex < static_cast<int>(fields.size()) &&
      parseSignedInt(fields[static_cast<size_t>(format.boldIndex)],
                     &parsedInt)) {
    style.bold = parsedInt != 0;
  }
  if (format.italicIndex >= 0 &&
      format.italicIndex < static_cast<int>(fields.size()) &&
      parseSignedInt(fields[static_cast<size_t>(format.italicIndex)],
                     &parsedInt)) {
    style.italic = parsedInt != 0;
  }
  if (format.underlineIndex >= 0 &&
      format.underlineIndex < static_cast<int>(fields.size()) &&
      parseSignedInt(fields[static_cast<size_t>(format.underlineIndex)],
                     &parsedInt)) {
    style.underline = parsedInt != 0;
  }
  if (format.scaleXIndex >= 0 &&
      format.scaleXIndex < static_cast<int>(fields.size()) &&
      parseFloatAscii(fields[static_cast<size_t>(format.scaleXIndex)],
                      &parsedFloat) &&
      parsedFloat > 0.0f) {
    style.scaleX = std::clamp(parsedFloat * 0.01f, 0.40f, 3.5f);
  }
  if (format.scaleYIndex >= 0 &&
      format.scaleYIndex < static_cast<int>(fields.size()) &&
      parseFloatAscii(fields[static_cast<size_t>(format.scaleYIndex)],
                      &parsedFloat) &&
      parsedFloat > 0.0f) {
    style.scaleY = std::clamp(parsedFloat * 0.01f, 0.40f, 3.5f);
  }
  if (format.primaryColourIndex >= 0 &&
      format.primaryColourIndex < static_cast<int>(fields.size()) &&
      parseAssColorValue(fields[static_cast<size_t>(format.primaryColourIndex)],
                         &style.primaryR, &style.primaryG, &style.primaryB,
                         &style.primaryAlpha)) {
    style.hasPrimaryColor = true;
  }
  if (format.backColourIndex >= 0 &&
      format.backColourIndex < static_cast<int>(fields.size()) &&
      parseAssColorValue(fields[static_cast<size_t>(format.backColourIndex)],
                         &style.backR, &style.backG, &style.backB,
                         &style.backAlpha)) {
    style.hasBackColor = true;
  }

  *outName = std::move(styleName);
  *outStyle = std::move(style);
  return true;
}

void parseAssScriptContext(const std::string& raw, AssScriptContext* outContext) {
  if (!outContext) return;
  *outContext = AssScriptContext{};

  std::vector<std::string> lines = splitLinesNormalized(raw);
  AssSection section = AssSection::Other;
  AssStyleFormatState styleFormat;

  for (const std::string& rawLine : lines) {
    const std::string line = trimAscii(rawLine);
    if (line.empty()) continue;

    if (line.front() == '[' && line.back() == ']') {
      section = assSectionFromLine(line);
      continue;
    }

    if (section == AssSection::ScriptInfo) {
      const size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      const std::string key = toLowerAscii(trimAscii(line.substr(0, colon)));
      const std::string value = trimAscii(line.substr(colon + 1));
      float parsedFloat = 0.0f;
      if (key == "playresx" && parseFloatAscii(value, &parsedFloat) &&
          parsedFloat > 0.0f) {
        outContext->playResX = static_cast<int>(std::lround(parsedFloat));
      } else if (key == "playresy" && parseFloatAscii(value, &parsedFloat) &&
                 parsedFloat > 0.0f) {
        outContext->playResY = static_cast<int>(std::lround(parsedFloat));
      }
      continue;
    }

    if (section != AssSection::Styles) continue;

    constexpr std::string_view kFormatPrefix = "Format:";
    constexpr std::string_view kStylePrefix = "Style:";
    if (startsWithAsciiNoCase(line, 0, kFormatPrefix)) {
      parseAssStyleFormat(
          std::string_view(line).substr(kFormatPrefix.size()), &styleFormat);
      continue;
    }
    if (!startsWithAsciiNoCase(line, 0, kStylePrefix)) continue;

    std::string styleName;
    AssStyleInfo style;
    if (!parseAssStyleLine(std::string_view(line).substr(kStylePrefix.size()),
                           styleFormat, &styleName, &style)) {
      continue;
    }

    const std::string nameLower = toLowerAscii(styleName);
    outContext->styles[nameLower] = style;
    if (outContext->baseFontSize <= 0.0f && style.fontSize > 0.0f) {
      outContext->baseFontSize = style.fontSize;
    }
    if (nameLower == "default" && style.fontSize > 0.0f) {
      outContext->baseFontSize = style.fontSize;
    }
  }
}

bool parseAssCues(const std::string& raw, std::vector<SubtitleCue>* outCues) {
  if (!outCues) return false;
  std::vector<std::string> lines = splitLinesNormalized(raw);
  if (lines.empty()) return false;

  AssSection section = AssSection::Other;

  AssScriptContext assContext;
  parseAssScriptContext(raw, &assContext);
  const int playResX = assContext.playResX;
  const int playResY = assContext.playResY;
  const float baseFontSize = assContext.baseFontSize;
  const std::unordered_map<std::string, AssStyleInfo>& styles =
      assContext.styles;

  std::vector<std::string> eventFormatFields;
  int startIndex = -1;
  int endIndex = -1;
  int textIndex = -1;
  int styleIndex = -1;
  int layerIndex = -1;
  int marginLIndex = -1;
  int marginRIndex = -1;
  int marginVIndex = -1;

  for (const std::string& rawLine : lines) {
    std::string line = trimAscii(rawLine);
    if (line.empty()) continue;

    if (line.front() == '[' && line.back() == ']') {
      section = assSectionFromLine(line);
      continue;
    }

    if (section == AssSection::ScriptInfo || section == AssSection::Styles) {
      continue;
    }
    if (section != AssSection::Events) continue;

    const std::string formatPrefix = "Format:";
    const std::string dialoguePrefix = "Dialogue:";
    if (line.size() >= formatPrefix.size() &&
        startsWith(toLowerAscii(line.substr(0, formatPrefix.size())),
                   toLowerAscii(formatPrefix))) {
      parseAssFormatFields(line.substr(formatPrefix.size()), &eventFormatFields);
      startIndex = findAssFieldIndex(eventFormatFields, "start");
      endIndex = findAssFieldIndex(eventFormatFields, "end");
      textIndex = findAssFieldIndex(eventFormatFields, "text");
      styleIndex = findAssFieldIndex(eventFormatFields, "style");
      layerIndex = findAssFieldIndex(eventFormatFields, "layer");
      marginLIndex = findAssFieldIndex(eventFormatFields, "marginl");
      marginRIndex = findAssFieldIndex(eventFormatFields, "marginr");
      marginVIndex = findAssFieldIndex(eventFormatFields, "marginv");
      continue;
    }

    if (line.size() < dialoguePrefix.size() ||
        !startsWith(toLowerAscii(line.substr(0, dialoguePrefix.size())),
                    toLowerAscii(dialoguePrefix)) ||
        eventFormatFields.empty() || startIndex < 0 || endIndex < 0 ||
        textIndex < 0) {
      continue;
    }

    std::vector<std::string> fields;
    if (!splitCsvPrefix(line.substr(dialoguePrefix.size()),
                        eventFormatFields.size(), &fields)) {
      continue;
    }
    if (startIndex >= static_cast<int>(fields.size()) ||
        endIndex >= static_cast<int>(fields.size()) ||
        textIndex >= static_cast<int>(fields.size())) {
      continue;
    }

    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!parseTimecodeUs(fields[static_cast<size_t>(startIndex)], &startUs) ||
        !parseTimecodeUs(fields[static_cast<size_t>(endIndex)], &endUs)) {
      continue;
    }
    if (endUs <= startUs) {
      endUs = startUs + 100000;
    }

    AssStyleInfo styleInfo;
    float cueFontSize = 0.0f;
    if (styleIndex >= 0 && styleIndex < static_cast<int>(fields.size())) {
      std::string styleName =
          toLowerAscii(trimAscii(fields[static_cast<size_t>(styleIndex)]));
      auto it = styles.find(styleName);
      if (it != styles.end()) {
        styleInfo = it->second;
        cueFontSize = it->second.fontSize;
      }
    }

    int parsedInt = 0;
    if (layerIndex >= 0 && layerIndex < static_cast<int>(fields.size()) &&
        parseNonNegativeInt(fields[static_cast<size_t>(layerIndex)], &parsedInt)) {
      // parsed below
    } else {
      parsedInt = 0;
    }
    const int cueLayer = parsedInt;

    if (marginLIndex >= 0 && marginLIndex < static_cast<int>(fields.size()) &&
        parseNonNegativeInt(fields[static_cast<size_t>(marginLIndex)], &parsedInt) &&
        parsedInt > 0) {
      styleInfo.marginL = parsedInt;
    }
    if (marginRIndex >= 0 && marginRIndex < static_cast<int>(fields.size()) &&
        parseNonNegativeInt(fields[static_cast<size_t>(marginRIndex)], &parsedInt) &&
        parsedInt > 0) {
      styleInfo.marginR = parsedInt;
    }
    if (marginVIndex >= 0 && marginVIndex < static_cast<int>(fields.size()) &&
        parseNonNegativeInt(fields[static_cast<size_t>(marginVIndex)], &parsedInt) &&
        parsedInt > 0) {
      styleInfo.marginV = parsedInt;
    }

    std::string rawText = fields[static_cast<size_t>(textIndex)];
    const AssStyleInfo textRunBaseStyle = styleInfo;
    AssOverrideInfo overrides;
    parseAssOverridesFromText(rawText, &overrides);
    if (overrides.alignment >= 1) {
      styleInfo.alignment = clampAssAlignment(overrides.alignment);
    }
    if (overrides.fontSize > 0.0f) {
      cueFontSize = overrides.fontSize;
    }
    if (overrides.scaleX > 0.0f) {
      styleInfo.scaleX = overrides.scaleX;
    }
    if (overrides.scaleY > 0.0f) {
      styleInfo.scaleY = overrides.scaleY;
    }
    if (overrides.hasFontName) {
      styleInfo.fontName = overrides.fontName;
    }
    if (overrides.hasBold) {
      styleInfo.bold = overrides.bold;
    }
    if (overrides.hasItalic) {
      styleInfo.italic = overrides.italic;
    }
    if (overrides.hasUnderline) {
      styleInfo.underline = overrides.underline;
    }
    if (overrides.hasPrimaryColor) {
      styleInfo.hasPrimaryColor = true;
      styleInfo.primaryR = overrides.primaryR;
      styleInfo.primaryG = overrides.primaryG;
      styleInfo.primaryB = overrides.primaryB;
      styleInfo.primaryAlpha = overrides.primaryAlpha;
    }
    if (overrides.hasBackColor) {
      styleInfo.hasBackColor = true;
      styleInfo.backR = overrides.backR;
      styleInfo.backG = overrides.backG;
      styleInfo.backB = overrides.backB;
      styleInfo.backAlpha = overrides.backAlpha;
    }

    std::string text;
    std::vector<SubtitleTextRun> textRuns;
    parseAssStyledTextRuns(rawText, textRunBaseStyle, &styles, &text,
                           &textRuns);
    if (text.empty()) continue;

    const int scriptW = std::max(1, playResX > 0 ? playResX : 384);
    const int scriptH = std::max(1, playResY > 0 ? playResY : 288);
    const float resolvedBaseFont = (baseFontSize > 0.0f)
                                       ? baseFontSize
                                       : (cueFontSize > 0.0f ? cueFontSize : 0.0f);

    SubtitleCue cue;
    cue.startUs = startUs;
    cue.endUs = endUs;
    cue.text = std::move(text);
    cue.rawText = rawText;
    cue.textRuns = std::move(textRuns);
    cue.assStyled = true;
    cue.layer = cueLayer;
    cue.alignment = clampAssAlignment(styleInfo.alignment);
    cue.marginLNorm = std::clamp(
        static_cast<float>(std::max(0, styleInfo.marginL)) / scriptW, 0.0f, 0.7f);
    cue.marginRNorm = std::clamp(
        static_cast<float>(std::max(0, styleInfo.marginR)) / scriptW, 0.0f, 0.7f);
    cue.marginVNorm = std::clamp(
        static_cast<float>(std::max(0, styleInfo.marginV)) / scriptH, 0.0f, 0.7f);
    cue.sizeScale = 1.0f;
    if (cueFontSize > 0.0f && resolvedBaseFont > 0.0f) {
      cue.sizeScale = std::clamp(cueFontSize / resolvedBaseFont, 0.45f, 2.5f);
    }
    cue.scaleX = std::clamp(styleInfo.scaleX, 0.40f, 3.5f);
    cue.scaleY = std::clamp(styleInfo.scaleY, 0.40f, 3.5f);
    cue.sizeScale = std::clamp(cue.sizeScale * cue.scaleY, 0.35f, 3.5f);
    cue.fontName = styleInfo.fontName;
    cue.bold = styleInfo.bold;
    cue.italic = styleInfo.italic;
    cue.underline = styleInfo.underline;
    cue.hasPrimaryColor = styleInfo.hasPrimaryColor;
    cue.primaryR = styleInfo.primaryR;
    cue.primaryG = styleInfo.primaryG;
    cue.primaryB = styleInfo.primaryB;
    cue.primaryAlpha = styleInfo.primaryAlpha;
    cue.hasBackColor = styleInfo.hasBackColor;
    cue.backR = styleInfo.backR;
    cue.backG = styleInfo.backG;
    cue.backB = styleInfo.backB;
    cue.backAlpha = styleInfo.backAlpha;
    if (overrides.hasPosition) {
      cue.hasPosition = true;
      cue.posXNorm =
          std::clamp(overrides.posX / static_cast<float>(scriptW), 0.0f, 1.0f);
      cue.posYNorm =
          std::clamp(overrides.posY / static_cast<float>(scriptH), 0.0f, 1.0f);
    }
    if (overrides.hasMove) {
      cue.hasMove = true;
      cue.hasPosition = true;
      cue.moveStartXNorm = std::clamp(
          overrides.moveStartX / static_cast<float>(scriptW), 0.0f, 1.0f);
      cue.moveStartYNorm = std::clamp(
          overrides.moveStartY / static_cast<float>(scriptH), 0.0f, 1.0f);
      cue.moveEndXNorm = std::clamp(
          overrides.moveEndX / static_cast<float>(scriptW), 0.0f, 1.0f);
      cue.moveEndYNorm = std::clamp(
          overrides.moveEndY / static_cast<float>(scriptH), 0.0f, 1.0f);
      cue.moveStartMs = std::max(0, overrides.moveStartMs);
      cue.moveEndMs = std::max(0, overrides.moveEndMs);
      cue.posXNorm = cue.moveStartXNorm;
      cue.posYNorm = cue.moveStartYNorm;
    }
    outCues->push_back(std::move(cue));
  }
  return !outCues->empty();
}

bool loadSubtitleTrackFile(const std::filesystem::path& path,
                           SubtitleTrack* outTrack) {
  auto tryParser = [](const std::string& payload,
                      bool (*parser)(const std::string&, std::vector<SubtitleCue>*),
                      std::vector<SubtitleCue>* outCues) {
    if (!parser || !outCues) return false;
    std::vector<SubtitleCue> parsed;
    if (!parser(payload, &parsed) || parsed.empty()) return false;
    *outCues = std::move(parsed);
    return true;
  };

  auto parseBySniffing = [&](const std::string& payload,
                             std::vector<SubtitleCue>* outCues) {
    if (tryParser(payload, &parseSrtCues, outCues)) return true;
    if (tryParser(payload, &parseVttCues, outCues)) return true;
    if (tryParser(payload, &parseAssCues, outCues)) {
      return true;
    }
    if (tryParser(payload, &parseSbvCues, outCues)) return true;
    if (tryParser(payload, &parseMicroDvdCues, outCues)) return true;
    return false;
  };

  if (!outTrack) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  std::string rawBytes((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  if (rawBytes.empty()) return false;
  std::string raw = decodeSubtitleText(rawBytes);
  if (raw.empty()) return false;

  std::string ext = toLowerAscii(toUtf8String(path.extension()));
  bool ok = false;
  if (ext == ".srt") {
    ok = tryParser(raw, &parseSrtCues, &outTrack->cues);
  } else if (ext == ".vtt") {
    ok = tryParser(raw, &parseVttCues, &outTrack->cues);
  } else if (ext == ".ass" || ext == ".ssa") {
    ok = tryParser(raw, &parseAssCues, &outTrack->cues);
    if (ok) {
      outTrack->assScript = std::make_shared<const std::string>(raw);
    }
  } else if (ext == ".sbv") {
    ok = tryParser(raw, &parseSbvCues, &outTrack->cues);
  } else if (ext == ".sub") {
    ok = tryParser(raw, &parseMicroDvdCues, &outTrack->cues);
  } else if (ext == ".txt" || ext == ".smi" || ext == ".sami") {
    ok = parseBySniffing(raw, &outTrack->cues);
  } else {
    ok = parseBySniffing(raw, &outTrack->cues);
  }
  if (!ok) {
    ok = parseBySniffing(raw, &outTrack->cues);
  }
  if (ok && !outTrack->assScript && !outTrack->cues.empty()) {
    const bool looksAss = std::all_of(
        outTrack->cues.begin(), outTrack->cues.end(),
        [](const SubtitleCue& cue) { return cue.assStyled; });
    if (looksAss) {
      outTrack->assScript = std::make_shared<const std::string>(raw);
    }
  }
  if (!ok || outTrack->cues.empty()) {
    outTrack->cues.clear();
    return false;
  }

  std::sort(outTrack->cues.begin(), outTrack->cues.end(),
            [](const SubtitleCue& a, const SubtitleCue& b) {
              if (a.startUs != b.startUs) return a.startUs < b.startUs;
              return a.endUs < b.endUs;
            });
  outTrack->resetLookup();
  return true;
}

bool isSubtitleSidecarExtension(const std::string& extLower) {
  return extLower == ".srt" || extLower == ".vtt" || extLower == ".ass" ||
         extLower == ".ssa" || extLower == ".sbv" || extLower == ".sub" ||
         extLower == ".txt" || extLower == ".smi" || extLower == ".sami";
}

bool isSubtitleDirectoryName(const std::string& nameLower) {
  return nameLower == "subs" || nameLower == "sub" || nameLower == "subtitle" ||
         nameLower == "subtitles";
}

std::vector<std::filesystem::path> discoverSubtitleFiles(
    const std::filesystem::path& videoPath) {
  std::vector<std::pair<int, std::filesystem::path>> ranked;
  std::vector<std::filesystem::path> fallbackCandidates;
  std::error_code ec;
  std::filesystem::path dir = videoPath.parent_path();
  if (dir.empty()) {
    dir = radioifyLaunchDir();
  }
  std::string baseStem = toUtf8String(videoPath.stem());
  std::string baseStemLower = toLowerAscii(baseStem);

  auto rankForStem = [&](const std::string& stem) -> int {
    if (baseStemLower.empty()) return -1;
    const std::string stemLower = toLowerAscii(stem);
    if (stemLower == baseStemLower) return 0;
    if (startsWith(stemLower, baseStemLower + ".")) return 1;
    if (startsWith(stemLower, baseStemLower + "_")) return 1;
    if (startsWith(stemLower, baseStemLower + "-")) return 1;
    if (startsWith(stemLower, baseStemLower + " ")) return 1;
    if (startsWith(stemLower, baseStemLower + "(")) return 1;
    if (stemLower.find(baseStemLower) != std::string::npos) return 3;
    return -1;
  };

  struct SearchDir {
    std::filesystem::path path;
    int maxDepth = 0;
  };
  std::vector<SearchDir> searchDirs;
  auto addSearchDir = [&](const std::filesystem::path& searchDir, int maxDepth) {
    std::string key = toLowerAscii(toUtf8String(searchDir.lexically_normal()));
    for (const auto& existing : searchDirs) {
      if (toLowerAscii(toUtf8String(existing.path.lexically_normal())) == key) {
        return;
      }
    }
    searchDirs.push_back(SearchDir{searchDir, std::max(0, maxDepth)});
  };

  addSearchDir(dir, 0);
  for (std::filesystem::directory_iterator it(dir, ec), end; it != end;
       it.increment(ec)) {
    if (ec) break;
    const auto& entry = *it;
    bool isDir = entry.is_directory(ec);
    if (ec) {
      ec.clear();
      continue;
    }
    if (!isDir) continue;
    std::string nameLower = toLowerAscii(toUtf8String(entry.path().filename()));
    if (isSubtitleDirectoryName(nameLower)) {
      addSearchDir(entry.path(), 2);
    }
  }
  if (ec) ec.clear();

  auto maybeAddCandidate = [&](const std::filesystem::path& candidate) {
    std::string extLower = toLowerAscii(toUtf8String(candidate.extension()));
    if (!isSubtitleSidecarExtension(extLower)) return;
    int rank = rankForStem(toUtf8String(candidate.stem()));
    if (rank >= 0) {
      ranked.emplace_back(rank, candidate);
    } else {
      fallbackCandidates.push_back(candidate);
    }
  };

  auto scanFlatDirectory = [&](const std::filesystem::path& searchDir) {
    for (std::filesystem::directory_iterator it(searchDir, ec), end; it != end;
         it.increment(ec)) {
      if (ec) break;
      const auto& entry = *it;
      bool isRegular = entry.is_regular_file(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      if (!isRegular) continue;
      maybeAddCandidate(entry.path());
    }
    if (ec) ec.clear();
  };

  auto scanRecursiveDirectory = [&](const std::filesystem::path& searchDir,
                                    int maxDepth) {
    std::filesystem::directory_options options =
        std::filesystem::directory_options::skip_permission_denied;
    for (std::filesystem::recursive_directory_iterator it(searchDir, options, ec),
         end;
         it != end; it.increment(ec)) {
      if (ec) {
        ec.clear();
        continue;
      }
      const auto& entry = *it;
      bool isDir = entry.is_directory(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      if (isDir) {
        if (it.depth() >= maxDepth) {
          it.disable_recursion_pending();
        }
        continue;
      }
      bool isRegular = entry.is_regular_file(ec);
      if (ec) {
        ec.clear();
        continue;
      }
      if (!isRegular) continue;
      maybeAddCandidate(entry.path());
    }
    if (ec) ec.clear();
  };

  for (const auto& searchDir : searchDirs) {
    bool dirExists = std::filesystem::exists(searchDir.path, ec);
    if (ec || !dirExists) {
      ec.clear();
      continue;
    }
    bool isDir = std::filesystem::is_directory(searchDir.path, ec);
    if (ec || !isDir) {
      ec.clear();
      continue;
    }
    if (searchDir.maxDepth <= 0) {
      scanFlatDirectory(searchDir.path);
    } else {
      scanRecursiveDirectory(searchDir.path, searchDir.maxDepth);
    }
  }

  if (ranked.empty()) {
    for (const auto& candidate : fallbackCandidates) {
      ranked.emplace_back(100, candidate);
    }
  }

  std::sort(ranked.begin(), ranked.end(),
            [](const auto& a, const auto& b) {
              if (a.first != b.first) return a.first < b.first;
              return toLowerAscii(toUtf8String(a.second)) <
                     toLowerAscii(toUtf8String(b.second));
            });

  ranked.erase(std::unique(ranked.begin(), ranked.end(),
                           [](const auto& a, const auto& b) {
                             return toLowerAscii(toUtf8String(a.second)) ==
                                    toLowerAscii(toUtf8String(b.second));
                           }),
               ranked.end());

  std::vector<std::filesystem::path> out;
  out.reserve(ranked.size());
  for (const auto& item : ranked) {
    out.push_back(item.second);
  }
  return out;
}

std::string subtitleLabelFromPath(const std::filesystem::path& path,
                                  const std::string& baseStem) {
  std::string stem = toUtf8String(path.stem());
  std::string label;
  std::string prefix = baseStem + ".";
  if (stem.rfind(prefix, 0) == 0 && stem.size() > prefix.size()) {
    label = stem.substr(prefix.size());
  } else {
    label = toUtf8String(path.extension());
    if (!label.empty() && label[0] == '.') {
      label.erase(0, 1);
    }
  }
  if (label.empty()) label = "Track";
  for (char& ch : label) {
    if (ch == '_' || ch == '-') ch = ' ';
  }
  return label;
}

std::string subtitleStreamMetadata(const AVStream* stream, const char* key) {
  if (!stream || !stream->metadata || !key) return {};
  const AVDictionaryEntry* entry =
      av_dict_get(stream->metadata, key, nullptr, 0);
  if (!entry || !entry->value || entry->value[0] == '\0') return {};
  return std::string(entry->value);
}

std::string subtitleLabelFromStream(const AVStream* stream, size_t ordinal) {
  std::string language = subtitleStreamMetadata(stream, "language");
  std::string title = subtitleStreamMetadata(stream, "title");
  if (!title.empty() && !language.empty()) {
    return language + " - " + title;
  }
  if (!language.empty()) return language;
  if (!title.empty()) return title;
  return "Embedded " + std::to_string(ordinal + 1);
}

bool isTextSubtitleCodec(AVCodecID codecId) {
  switch (codecId) {
    case AV_CODEC_ID_TEXT:
    case AV_CODEC_ID_ASS:
    case AV_CODEC_ID_SSA:
    case AV_CODEC_ID_SUBRIP:
    case AV_CODEC_ID_WEBVTT:
    case AV_CODEC_ID_MOV_TEXT:
#ifdef AV_CODEC_ID_MICRODVD
    case AV_CODEC_ID_MICRODVD:
#endif
#ifdef AV_CODEC_ID_JACOSUB
    case AV_CODEC_ID_JACOSUB:
#endif
#ifdef AV_CODEC_ID_SUBVIEWER
    case AV_CODEC_ID_SUBVIEWER:
#endif
#ifdef AV_CODEC_ID_SUBVIEWER1
    case AV_CODEC_ID_SUBVIEWER1:
#endif
      return true;
    default:
      return false;
  }
}

int64_t packetPtsUs(const AVPacket& pkt, AVRational timeBase) {
  int64_t pts = pkt.pts;
  if (pts == AV_NOPTS_VALUE) pts = pkt.dts;
  if (pts == AV_NOPTS_VALUE) return AV_NOPTS_VALUE;
  return av_rescale_q(pts, timeBase, AVRational{1, 1000000});
}

int64_t packetDurationUs(const AVPacket& pkt, AVRational timeBase) {
  if (pkt.duration <= 0) return 0;
  int64_t durUs = av_rescale_q(pkt.duration, timeBase, AVRational{1, 1000000});
  return durUs > 0 ? durUs : 0;
}

bool containsAsciiInsensitive(std::string_view haystack, std::string_view needle) {
  if (needle.empty() || haystack.size() < needle.size()) return false;
  for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < needle.size(); ++j) {
      unsigned char lhs = static_cast<unsigned char>(haystack[i + j]);
      unsigned char rhs = static_cast<unsigned char>(needle[j]);
      lhs = static_cast<unsigned char>(std::tolower(lhs));
      rhs = static_cast<unsigned char>(std::tolower(rhs));
      if (lhs != rhs) {
        match = false;
        break;
      }
    }
    if (match) return true;
  }
  return false;
}

void normalizeAssNewlines(std::string* text) {
  if (!text) return;
  std::string normalized;
  normalized.reserve(text->size());
  for (size_t i = 0; i < text->size(); ++i) {
    char ch = (*text)[i];
    if (ch == '\r') {
      if (i + 1 < text->size() && (*text)[i + 1] == '\n') {
        continue;
      }
      normalized.push_back('\n');
      continue;
    }
    normalized.push_back(ch);
  }
  *text = std::move(normalized);
}

std::string assTimestampFromUs(int64_t us) {
  if (us < 0) us = 0;
  const int64_t cs = us / 10000;
  const int64_t h = cs / 360000;
  const int64_t m = (cs / 6000) % 60;
  const int64_t s = (cs / 100) % 60;
  const int64_t cc = cs % 100;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%lld:%02lld:%02lld.%02lld",
                static_cast<long long>(h), static_cast<long long>(m),
                static_cast<long long>(s), static_cast<long long>(cc));
  return std::string(buf);
}

void ensureEmbeddedAssScriptPreamble(std::string* script) {
  if (!script) return;
  normalizeAssNewlines(script);
  if (!script->empty() && script->back() != '\n') {
    script->push_back('\n');
  }
  if (!containsAsciiInsensitive(*script, "[Script Info]")) {
    script->append("[Script Info]\n");
    script->append("ScriptType: v4.00+\n");
    script->append("PlayResX: 384\n");
    script->append("PlayResY: 288\n");
  }
  if (!containsAsciiInsensitive(*script, "[V4+ Styles]") &&
      !containsAsciiInsensitive(*script, "[V4 Styles]")) {
    script->append("\n[V4+ Styles]\n");
    script->append(
        "Format: Name,Fontname,Fontsize,PrimaryColour,SecondaryColour,"
        "OutlineColour,BackColour,Bold,Italic,Underline,StrikeOut,"
        "ScaleX,ScaleY,Spacing,Angle,BorderStyle,Outline,Shadow,"
        "Alignment,MarginL,MarginR,MarginV,Encoding\n");
    script->append(
        "Style: Default,Arial,48,&H00FFFFFF,&H000000FF,&H00000000,&H64000000,"
        "0,0,0,0,100,100,0,0,1,2,0,2,12,12,12,1\n");
  }
  if (!containsAsciiInsensitive(*script, "[Events]")) {
    script->append("\n[Events]\n");
  }
  if (!containsAsciiInsensitive(
          *script,
          "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text") &&
      !containsAsciiInsensitive(
          *script,
          "Format: Layer, Start, End, Style, Name, MarginL, MarginR, MarginV, Effect, Text")) {
    script->append(
        "Format: Layer,Start,End,Style,Name,MarginL,MarginR,MarginV,Effect,Text\n");
  }
}

struct AssPacketEventFields {
  int layer = 0;
  std::string style = "Default";
  std::string name;
  int marginL = 0;
  int marginR = 0;
  int marginV = 0;
  std::string effect;
  std::string text;
};

bool parseAssPacketEventLine(const std::string& payload,
                             AssPacketEventFields* outFields) {
  if (!outFields) return false;
  *outFields = AssPacketEventFields{};
  std::string line = trimAscii(payload);
  if (line.empty()) return false;

  std::string lower = toLowerAscii(line);
  if (startsWith(lower, "dialogue:")) {
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;
    line = trimAscii(line.substr(colon + 1));
  }
  if (line.empty()) return false;

  auto parseMargins = [&](const std::vector<std::string>& fields, int idxL,
                          int idxR, int idxV) {
    int parsed = 0;
    if (idxL >= 0 && idxL < static_cast<int>(fields.size()) &&
        parseNonNegativeInt(fields[static_cast<size_t>(idxL)], &parsed)) {
      outFields->marginL = parsed;
    }
    if (idxR >= 0 && idxR < static_cast<int>(fields.size()) &&
        parseNonNegativeInt(fields[static_cast<size_t>(idxR)], &parsed)) {
      outFields->marginR = parsed;
    }
    if (idxV >= 0 && idxV < static_cast<int>(fields.size()) &&
        parseNonNegativeInt(fields[static_cast<size_t>(idxV)], &parsed)) {
      outFields->marginV = parsed;
    }
  };

  std::vector<std::string> fields;
  if (splitCsvPrefix(line, 10u, &fields) && fields.size() >= 10u) {
    int parsedLayer = 0;
    int64_t startUs = 0;
    int64_t endUs = 0;
    if (parseSignedInt(fields[0], &parsedLayer) &&
        parseTimecodeUs(fields[1], &startUs) && parseTimecodeUs(fields[2], &endUs)) {
      outFields->layer = (std::max)(0, parsedLayer);
      outFields->style = trimAscii(fields[3]);
      if (outFields->style.empty()) outFields->style = "Default";
      outFields->name = trimAscii(fields[4]);
      parseMargins(fields, 5, 6, 7);
      outFields->effect = trimAscii(fields[8]);
      outFields->text = fields[9];
      return !outFields->text.empty();
    }
  }

  fields.clear();
  if (splitCsvPrefix(line, 9u, &fields) && fields.size() >= 9u) {
    int parsedLayer = 0;
    if (!parseSignedInt(fields[1], &parsedLayer)) return false;
    outFields->layer = (std::max)(0, parsedLayer);
    outFields->style = trimAscii(fields[2]);
    if (outFields->style.empty()) outFields->style = "Default";
    outFields->name = trimAscii(fields[3]);
    parseMargins(fields, 4, 5, 6);
    outFields->effect = trimAscii(fields[7]);
    outFields->text = fields[8];
    return !outFields->text.empty();
  }

  return false;
}

std::string assNormalizeDialogueText(std::string text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (char ch : text) {
    if (ch == '\r') continue;
    if (ch == '\n') {
      out += "\\N";
      continue;
    }
    out.push_back(ch);
  }
  return out;
}

void appendEmbeddedAssDialogue(std::string* script, int64_t startUs, int64_t endUs,
                               const AssPacketEventFields& event) {
  if (!script || event.text.empty()) return;
  if (endUs <= startUs) endUs = startUs + 100000;
  if (!script->empty() && script->back() != '\n') script->push_back('\n');

  script->append("Dialogue: ");
  script->append(std::to_string((std::max)(0, event.layer)));
  script->push_back(',');
  script->append(assTimestampFromUs(startUs));
  script->push_back(',');
  script->append(assTimestampFromUs(endUs));
  script->push_back(',');
  script->append(event.style.empty() ? "Default" : event.style);
  script->push_back(',');
  script->append(event.name);
  script->push_back(',');
  script->append(std::to_string((std::max)(0, event.marginL)));
  script->push_back(',');
  script->append(std::to_string((std::max)(0, event.marginR)));
  script->push_back(',');
  script->append(std::to_string((std::max)(0, event.marginV)));
  script->push_back(',');
  script->append(event.effect);
  script->push_back(',');
  script->append(assNormalizeDialogueText(event.text));
  script->push_back('\n');
}

bool packetSubtitleCue(AVCodecID codecId, const AVPacket& pkt, SubtitleCue* outCue,
                       const AssScriptContext* assContext = nullptr,
                       AssPacketEventFields* outAssEvent = nullptr) {
  if (!outCue || !pkt.data || pkt.size <= 0) return false;
  std::string payload(reinterpret_cast<const char*>(pkt.data),
                      reinterpret_cast<const char*>(pkt.data + pkt.size));

  SubtitleCue cue;
  if (codecId == AV_CODEC_ID_ASS || codecId == AV_CODEC_ID_SSA) {
    cue.assStyled = true;
    AssPacketEventFields event;
    if (!parseAssPacketEventLine(payload, &event)) return false;
    if (outAssEvent) {
      *outAssEvent = event;
    }
    cue.layer = event.layer;
    payload = event.text;

    AssStyleInfo styleInfo;
    float cueFontSize = 0.0f;
    const std::unordered_map<std::string, AssStyleInfo>* styles = nullptr;
    if (assContext) {
      styles = &assContext->styles;
      const std::string styleName = toLowerAscii(trimAscii(event.style));
      auto it = assContext->styles.find(styleName);
      if (it != assContext->styles.end()) {
        styleInfo = it->second;
        cueFontSize = it->second.fontSize;
      }
    }

    if (event.marginL > 0) styleInfo.marginL = event.marginL;
    if (event.marginR > 0) styleInfo.marginR = event.marginR;
    if (event.marginV > 0) styleInfo.marginV = event.marginV;

    const AssStyleInfo textRunBaseStyle = styleInfo;
    AssOverrideInfo overrides;
    parseAssOverridesFromText(payload, &overrides);
    if (overrides.alignment >= 1) {
      styleInfo.alignment = clampAssAlignment(overrides.alignment);
    }
    if (overrides.fontSize > 0.0f) {
      cueFontSize = overrides.fontSize;
    }
    if (overrides.scaleX > 0.0f) {
      styleInfo.scaleX = overrides.scaleX;
    }
    if (overrides.scaleY > 0.0f) {
      styleInfo.scaleY = overrides.scaleY;
    }
    if (overrides.hasFontName) {
      styleInfo.fontName = overrides.fontName;
    }
    if (overrides.hasBold) {
      styleInfo.bold = overrides.bold;
    }
    if (overrides.hasItalic) {
      styleInfo.italic = overrides.italic;
    }
    if (overrides.hasUnderline) {
      styleInfo.underline = overrides.underline;
    }
    if (overrides.hasPrimaryColor) {
      styleInfo.hasPrimaryColor = true;
      styleInfo.primaryR = overrides.primaryR;
      styleInfo.primaryG = overrides.primaryG;
      styleInfo.primaryB = overrides.primaryB;
      styleInfo.primaryAlpha = overrides.primaryAlpha;
    }
    if (overrides.hasBackColor) {
      styleInfo.hasBackColor = true;
      styleInfo.backR = overrides.backR;
      styleInfo.backG = overrides.backG;
      styleInfo.backB = overrides.backB;
      styleInfo.backAlpha = overrides.backAlpha;
    }

    const int scriptW =
        std::max(1, assContext && assContext->playResX > 0
                        ? assContext->playResX
                        : 384);
    const int scriptH =
        std::max(1, assContext && assContext->playResY > 0
                        ? assContext->playResY
                        : 288);
    const float baseFontSize =
        assContext && assContext->baseFontSize > 0.0f
            ? assContext->baseFontSize
            : (cueFontSize > 0.0f ? cueFontSize : 48.0f);

    cue.alignment = clampAssAlignment(styleInfo.alignment);
    cue.marginLNorm = std::clamp(
        static_cast<float>(std::max(0, styleInfo.marginL)) /
            static_cast<float>(scriptW),
        0.0f, 0.7f);
    cue.marginRNorm = std::clamp(
        static_cast<float>(std::max(0, styleInfo.marginR)) /
            static_cast<float>(scriptW),
        0.0f, 0.7f);
    cue.marginVNorm = std::clamp(
        static_cast<float>(std::max(0, styleInfo.marginV)) /
            static_cast<float>(scriptH),
        0.0f, 0.7f);
    if (cueFontSize > 0.0f && baseFontSize > 0.0f) {
      cue.sizeScale = std::clamp(cueFontSize / baseFontSize, 0.45f, 2.5f);
    }
    cue.scaleX = std::clamp(styleInfo.scaleX, 0.40f, 3.5f);
    cue.scaleY = std::clamp(styleInfo.scaleY, 0.40f, 3.5f);
    cue.sizeScale = std::clamp(cue.sizeScale * cue.scaleY, 0.35f, 3.5f);
    cue.fontName = styleInfo.fontName;
    cue.bold = styleInfo.bold;
    cue.italic = styleInfo.italic;
    cue.underline = styleInfo.underline;
    cue.hasPrimaryColor = styleInfo.hasPrimaryColor;
    cue.primaryR = styleInfo.primaryR;
    cue.primaryG = styleInfo.primaryG;
    cue.primaryB = styleInfo.primaryB;
    cue.primaryAlpha = styleInfo.primaryAlpha;
    cue.hasBackColor = styleInfo.hasBackColor;
    cue.backR = styleInfo.backR;
    cue.backG = styleInfo.backG;
    cue.backB = styleInfo.backB;
    cue.backAlpha = styleInfo.backAlpha;
    if (overrides.hasPosition) {
      cue.hasPosition = true;
      cue.posXNorm = std::clamp(
          overrides.posX / static_cast<float>(scriptW), 0.0f, 1.0f);
      cue.posYNorm = std::clamp(
          overrides.posY / static_cast<float>(scriptH), 0.0f, 1.0f);
    }
    if (overrides.hasMove) {
      cue.hasMove = true;
      cue.hasPosition = true;
      cue.moveStartXNorm = std::clamp(
          overrides.moveStartX / static_cast<float>(scriptW), 0.0f, 1.0f);
      cue.moveStartYNorm = std::clamp(
          overrides.moveStartY / static_cast<float>(scriptH), 0.0f, 1.0f);
      cue.moveEndXNorm = std::clamp(
          overrides.moveEndX / static_cast<float>(scriptW), 0.0f, 1.0f);
      cue.moveEndYNorm = std::clamp(
          overrides.moveEndY / static_cast<float>(scriptH), 0.0f, 1.0f);
      cue.moveStartMs = std::max(0, overrides.moveStartMs);
      cue.moveEndMs = std::max(0, overrides.moveEndMs);
      cue.posXNorm = cue.moveStartXNorm;
      cue.posYNorm = cue.moveStartYNorm;
    }

    std::string styledText;
    std::vector<SubtitleTextRun> textRuns;
    parseAssStyledTextRuns(payload, textRunBaseStyle, styles, &styledText,
                           &textRuns);
    cue.textRuns = std::move(textRuns);
    cue.text = std::move(styledText);

  }
  cue.rawText = payload;
  if (cue.text.empty()) {
    replaceAll(&payload, "\\N", "\n");
    replaceAll(&payload, "\\n", "\n");
    cue.text = stripSubtitleMarkup(payload);
  }

  if (cue.text.empty()) return false;
  *outCue = std::move(cue);
  return true;
}

bool loadEmbeddedSubtitleTracks(const std::filesystem::path& videoPath,
                                std::vector<SubtitleTrack>* outTracks) {
  if (!outTracks) return false;
  outTracks->clear();

  AVFormatContext* fmt = nullptr;
  std::string pathUtf8 = toUtf8String(videoPath);
  if (avformat_open_input(&fmt, pathUtf8.c_str(), nullptr, nullptr) < 0) {
    return false;
  }
  struct Scope {
    AVFormatContext** fmt = nullptr;
    ~Scope() {
      if (fmt && *fmt) avformat_close_input(fmt);
    }
  } scope{&fmt};

  (void)avformat_find_stream_info(fmt, nullptr);

  struct EmbeddedTrackInfo {
    int streamIndex = -1;
    AVRational timeBase{0, 1};
    AVCodecID codecId = AV_CODEC_ID_NONE;
    bool assLike = false;
    std::string assScript;
    AssScriptContext assContext;
    SubtitleTrack track;
  };
  std::vector<EmbeddedTrackInfo> refs;
  const std::shared_ptr<const SubtitleFontAttachmentList> fontAttachments =
      loadEmbeddedSubtitleFontAttachments(fmt);

  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    AVStream* stream = fmt->streams[i];
    if (!stream || !stream->codecpar) continue;
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
    EmbeddedTrackInfo info;
    info.streamIndex = static_cast<int>(i);
    info.timeBase = stream->time_base;
    info.codecId = stream->codecpar->codec_id;
    info.assLike =
        (info.codecId == AV_CODEC_ID_ASS || info.codecId == AV_CODEC_ID_SSA);
    if (info.assLike && stream->codecpar->extradata &&
        stream->codecpar->extradata_size > 0) {
      info.assScript.assign(
          reinterpret_cast<const char*>(stream->codecpar->extradata),
          reinterpret_cast<const char*>(stream->codecpar->extradata +
                                        stream->codecpar->extradata_size));
    }
    if (info.assLike) {
      ensureEmbeddedAssScriptPreamble(&info.assScript);
      parseAssScriptContext(info.assScript, &info.assContext);
    }
    info.track.label = subtitleLabelFromStream(stream, refs.size());
    refs.push_back(std::move(info));
  }

  if (refs.empty()) return false;
  int64_t formatStartUs =
      (fmt->start_time != AV_NOPTS_VALUE) ? fmt->start_time : 0;

  AVPacket* pkt = av_packet_alloc();
  if (!pkt) {
    for (auto& ref : refs) {
      ref.track.resetLookup();
      outTracks->push_back(std::move(ref.track));
    }
    return !outTracks->empty();
  }

  while (av_read_frame(fmt, pkt) >= 0) {
    for (auto& ref : refs) {
      if (pkt->stream_index != ref.streamIndex) continue;
      if (!isTextSubtitleCodec(ref.codecId)) break;
      SubtitleCue cue;
      AssPacketEventFields assEvent;
      AssPacketEventFields* assEventOut = ref.assLike ? &assEvent : nullptr;
      const AssScriptContext* assContext =
          ref.assLike ? &ref.assContext : nullptr;
      if (!packetSubtitleCue(ref.codecId, *pkt, &cue, assContext, assEventOut)) {
        break;
      }
      int64_t startUs = packetPtsUs(*pkt, ref.timeBase);
      if (startUs == AV_NOPTS_VALUE) break;
      startUs -= formatStartUs;
      if (startUs < 0) startUs = 0;
      int64_t endUs = startUs + packetDurationUs(*pkt, ref.timeBase);
      if (endUs <= startUs) {
        endUs = startUs + 2000000;  // 2 seconds fallback
      }
      cue.startUs = startUs;
      cue.endUs = endUs;
      if (ref.assLike) {
        appendEmbeddedAssDialogue(&ref.assScript, startUs, endUs, assEvent);
      }
      ref.track.cues.push_back(std::move(cue));
      break;
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);

  for (auto& ref : refs) {
    if (!ref.track.cues.empty()) {
      std::sort(ref.track.cues.begin(), ref.track.cues.end(),
                [](const SubtitleCue& a, const SubtitleCue& b) {
                  if (a.startUs != b.startUs) return a.startUs < b.startUs;
                  return a.endUs < b.endUs;
                });
    }
    if (ref.assLike && !ref.assScript.empty()) {
      ref.track.assScript = std::make_shared<const std::string>(ref.assScript);
      ref.track.assFonts = fontAttachments;
    }
    ref.track.resetLookup();
    outTracks->push_back(std::move(ref.track));
  }
  return !outTracks->empty();
}

}  // namespace

void SubtitleTrack::cuesAt(int64_t clockUs,
                           std::vector<const SubtitleCue*>* out) const {
  if (!out) return;
  out->clear();
  if (cues.empty()) return;

  auto it = std::upper_bound(cues.begin(), cues.end(), clockUs,
                             [](int64_t t, const SubtitleCue& cue) {
                               return t < cue.startUs;
                             });
  while (it != cues.begin()) {
    --it;
    if (clockUs >= it->startUs && clockUs < it->endUs) {
      out->push_back(&(*it));
    }
  }
  std::reverse(out->begin(), out->end());
  if (!out->empty()) {
    lastCueIndex = static_cast<size_t>(out->back() - cues.data());
  }
}

const SubtitleCue* SubtitleTrack::cueAt(int64_t clockUs) const {
  std::vector<const SubtitleCue*> active;
  cuesAt(clockUs, &active);
  if (active.empty()) return nullptr;
  return active.front();
}

void SubtitleTrack::resetLookup() const { lastCueIndex = 0; }

void SubtitleManager::loadForVideo(const std::filesystem::path& videoPath) {
  tracks_.clear();
  activeTrack_ = 0;

  const std::string baseStem = toUtf8String(videoPath.stem());
  std::vector<std::filesystem::path> files = discoverSubtitleFiles(videoPath);
  for (const auto& subtitleFile : files) {
    SubtitleTrack track;
    track.label = subtitleLabelFromPath(subtitleFile, baseStem);
    if (!loadSubtitleTrackFile(subtitleFile, &track)) {
      continue;
    }
    track.label = makeUniqueLabel(track.label);
    tracks_.push_back(std::move(track));
  }

  std::vector<SubtitleTrack> embeddedTracks;
  if (loadEmbeddedSubtitleTracks(videoPath, &embeddedTracks)) {
    for (auto& embedded : embeddedTracks) {
      embedded.label = makeUniqueLabel(embedded.label);
      tracks_.push_back(std::move(embedded));
    }
  }

  activeTrack_ = 0;
  selectFirstTrackWithCues();
}

size_t SubtitleManager::trackCount() const { return tracks_.size(); }

size_t SubtitleManager::selectableTrackCount() const {
  return static_cast<size_t>(std::count_if(
      tracks_.begin(), tracks_.end(),
      [](const SubtitleTrack& t) { return !t.cues.empty(); }));
}

bool SubtitleManager::selectFirstTrackWithCues() {
  for (size_t i = 0; i < tracks_.size(); ++i) {
    if (tracks_[i].cues.empty()) continue;
    activeTrack_ = i;
    tracks_[activeTrack_].resetLookup();
    return true;
  }
  return false;
}

size_t SubtitleManager::activeTrackIndex() const {
  if (tracks_.empty() || activeTrack_ >= tracks_.size() ||
      tracks_[activeTrack_].cues.empty()) {
    return static_cast<size_t>(-1);
  }
  return activeTrack_;
}

const SubtitleTrack* SubtitleManager::activeTrack() const {
  if (tracks_.empty() || activeTrack_ >= tracks_.size() ||
      tracks_[activeTrack_].cues.empty()) {
    return nullptr;
  }
  return &tracks_[activeTrack_];
}

std::string SubtitleManager::activeTrackLabel() const {
  if (tracks_.empty() || activeTrack_ >= tracks_.size() ||
      tracks_[activeTrack_].cues.empty()) {
    return "N/A";
  }
  return tracks_[activeTrack_].label.empty() ? "N/A"
                                             : tracks_[activeTrack_].label;
}

bool SubtitleManager::isActiveLastCueTrack() const {
  if (tracks_.empty() || activeTrack_ >= tracks_.size() ||
      tracks_[activeTrack_].cues.empty()) {
    return false;
  }
  for (size_t i = activeTrack_ + 1; i < tracks_.size(); ++i) {
    if (!tracks_[i].cues.empty()) return false;
  }
  return true;
}

bool SubtitleManager::cycleLanguage() {
  if (tracks_.empty()) return false;
  if (activeTrack_ >= tracks_.size()) activeTrack_ = 0;
  if (tracks_[activeTrack_].cues.empty() && !selectFirstTrackWithCues()) {
    return false;
  }
  if (tracks_[activeTrack_].cues.empty()) return false;

  for (size_t i = activeTrack_ + 1; i < tracks_.size(); ++i) {
    if (tracks_[i].cues.empty()) continue;
    activeTrack_ = i;
    tracks_[activeTrack_].resetLookup();
    return true;
  }
  return false;
}

std::string SubtitleManager::makeUniqueLabel(const std::string& rawLabel) const {
  std::string base = rawLabel.empty() ? "Track" : rawLabel;
  std::string candidate = base;
  int suffix = 2;
  auto exists = [&](const std::string& label) {
    return std::any_of(tracks_.begin(), tracks_.end(),
                       [&](const SubtitleTrack& t) { return t.label == label; });
  };
  while (exists(candidate)) {
    candidate = base + " " + std::to_string(suffix++);
  }
  return candidate;
}
