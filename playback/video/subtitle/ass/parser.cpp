#include "parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string_view>
#include <utility>

namespace {

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

}  // namespace

int clampAssAlignment(int alignment) {
  if (alignment < 1 || alignment > 9) return 2;
  return alignment;
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

bool parseAssAlphaValue(std::string_view token, int* outAlpha);

static std::vector<std::string> splitAssTagArgs(std::string_view args) {
  std::vector<std::string> parts;
  size_t start = 0;
  int depth = 0;
  for (size_t i = 0; i < args.size(); ++i) {
    if (args[i] == '(') {
      ++depth;
    } else if (args[i] == ')' && depth > 0) {
      --depth;
    } else if (args[i] == ',' && depth == 0) {
      parts.push_back(trimAscii(args.substr(start, i - start)));
      start = i + 1;
    }
  }
  parts.push_back(trimAscii(args.substr(start)));
  return parts;
}

static bool assTagArgContainsOverride(std::string_view arg) {
  return arg.find('\\') != std::string_view::npos;
}

static size_t findAssTagCloseParen(std::string_view text, size_t contentStart) {
  int depth = 1;
  for (size_t i = contentStart; i < text.size(); ++i) {
    if (text[i] == '(') {
      ++depth;
    } else if (text[i] == ')') {
      --depth;
      if (depth == 0) return i;
    }
  }
  return std::string_view::npos;
}

void appendSubtitleTransforms(const std::vector<AssTransformInfo>& source,
                              float baseFontSize, SubtitleCue* cue) {
  if (!cue || source.empty()) return;
  cue->transforms.reserve(cue->transforms.size() + source.size());
  for (const AssTransformInfo& src : source) {
    SubtitleTransform transform;
    transform.startMs = src.startMs;
    transform.endMs = src.endMs;
    transform.accel = src.accel;
    if (src.fontSize > 0.0f && baseFontSize > 0.0f) {
      transform.hasSizeScale = true;
      transform.sizeScale = std::clamp(src.fontSize / baseFontSize, 0.45f, 2.5f);
    }
    if (src.scaleX > 0.0f) {
      transform.hasScaleX = true;
      transform.scaleX = std::clamp(src.scaleX, 0.40f, 3.5f);
    }
    if (src.scaleY > 0.0f) {
      transform.hasScaleY = true;
      transform.scaleY = std::clamp(src.scaleY, 0.40f, 3.5f);
    }
    transform.hasPrimaryColor = src.hasPrimaryColor;
    transform.primaryR = src.primaryR;
    transform.primaryG = src.primaryG;
    transform.primaryB = src.primaryB;
    transform.hasPrimaryAlpha = src.hasPrimaryAlpha;
    transform.primaryAlpha = src.primaryAlpha;
    transform.hasBackColor = src.hasBackColor;
    transform.backR = src.backR;
    transform.backG = src.backG;
    transform.backB = src.backB;
    transform.hasBackAlpha = src.hasBackAlpha;
    transform.backAlpha = src.backAlpha;
    if (transform.hasSizeScale || transform.hasScaleX || transform.hasScaleY ||
        transform.hasPrimaryColor || transform.hasPrimaryAlpha ||
        transform.hasBackColor || transform.hasBackAlpha) {
      cue->transforms.push_back(transform);
    }
  }
}

static void parseAssOverrideBlock(std::string_view block, AssOverrideInfo* out) {
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
          out->hasBackAlpha = true;
          out->backR = r;
          out->backG = g;
          out->backB = b;
          out->backAlpha = alpha;
        } else {
          out->hasPrimaryColor = true;
          out->hasPrimaryAlpha = true;
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
    if (i + 2 <= block.size() &&
        (block[i] == '3' || block[i] == '4') &&
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
        if (primary) {
          out->hasPrimaryAlpha = true;
          out->primaryAlpha = alpha;
        }
        if (back) {
          out->hasBackAlpha = true;
          out->backAlpha = alpha;
        }
      }
    };

    if (i + 5 <= block.size() && startsWithAsciiNoCase(block, i, "alpha")) {
      parseAlphaToken(5, true, true);
      continue;
    }
    if (i + 2 <= block.size() && block[i] == '1' &&
        (block[i + 1] == 'a' || block[i + 1] == 'A')) {
      parseAlphaToken(2, true, false);
      continue;
    }
    if (i + 2 <= block.size() &&
        (block[i] == '3' || block[i] == '4') &&
        (block[i + 1] == 'a' || block[i + 1] == 'A')) {
      parseAlphaToken(2, false, true);
      continue;
    }

    if (startsWithAsciiNoCase(block, i, "fade")) {
      i += 4;
      if (i >= block.size() || block[i] != '(') continue;
      ++i;
      size_t end = block.find(')', i);
      if (end == std::string_view::npos) continue;
      std::vector<std::string> parts =
          splitAssTagArgs(block.substr(i, end - i));
      if (parts.size() >= 7) {
        int values[7]{};
        bool ok = true;
        for (int part = 0; part < 7; ++part) {
          ok = ok && parseSignedInt(parts[static_cast<size_t>(part)],
                                    &values[part]);
        }
        if (ok) {
          out->hasComplexFade = true;
          out->hasSimpleFade = false;
          out->fadeAlpha1 = std::clamp(values[0], 0, 255);
          out->fadeAlpha2 = std::clamp(values[1], 0, 255);
          out->fadeAlpha3 = std::clamp(values[2], 0, 255);
          out->fadeT1Ms = std::max(0, values[3]);
          out->fadeT2Ms = std::max(0, values[4]);
          out->fadeT3Ms = std::max(0, values[5]);
          out->fadeT4Ms = std::max(0, values[6]);
        }
      }
      i = end + 1;
      continue;
    }

    if (startsWithAsciiNoCase(block, i, "fad")) {
      i += 3;
      if (i >= block.size() || block[i] != '(') continue;
      ++i;
      size_t end = block.find(')', i);
      if (end == std::string_view::npos) continue;
      std::vector<std::string> parts =
          splitAssTagArgs(block.substr(i, end - i));
      int fadeIn = 0;
      int fadeOut = 0;
      if (parts.size() >= 2 && parseNonNegativeInt(parts[0], &fadeIn) &&
          parseNonNegativeInt(parts[1], &fadeOut)) {
        out->hasSimpleFade = true;
        out->hasComplexFade = false;
        out->fadeInMs = fadeIn;
        out->fadeOutMs = fadeOut;
      }
      i = end + 1;
      continue;
    }

    if ((block[i] == 't' || block[i] == 'T') &&
        i + 1 < block.size() && block[i + 1] == '(') {
      i += 2;
      size_t end = findAssTagCloseParen(block, i);
      if (end == std::string_view::npos) continue;
      std::vector<std::string> parts =
          splitAssTagArgs(block.substr(i, end - i));
      size_t overrideIndex = parts.size();
      for (size_t part = 0; part < parts.size(); ++part) {
        if (assTagArgContainsOverride(parts[part])) {
          overrideIndex = part;
          break;
        }
      }
      if (overrideIndex < parts.size()) {
        int startMs = 0;
        int endMs = 0;
        float accel = 1.0f;
        if (overrideIndex == 1) {
          float parsedAccel = 1.0f;
          if (parseFloatAscii(parts[0], &parsedAccel) && parsedAccel > 0.0f) {
            accel = parsedAccel;
          }
        } else if (overrideIndex == 2) {
          parseNonNegativeInt(parts[0], &startMs);
          parseNonNegativeInt(parts[1], &endMs);
        } else if (overrideIndex >= 3) {
          parseNonNegativeInt(parts[0], &startMs);
          parseNonNegativeInt(parts[1], &endMs);
          float parsedAccel = 1.0f;
          if (parseFloatAscii(parts[2], &parsedAccel) && parsedAccel > 0.0f) {
            accel = parsedAccel;
          }
        }

        std::string nested;
        for (size_t part = overrideIndex; part < parts.size(); ++part) {
          if (!nested.empty()) nested.push_back(',');
          nested.append(parts[part]);
        }
        AssOverrideInfo target;
        parseAssOverrideBlock(nested, &target);

        AssTransformInfo transform;
        transform.startMs = std::max(0, startMs);
        transform.endMs = std::max(0, endMs);
        transform.accel = std::clamp(accel, 0.1f, 8.0f);
        transform.fontSize = target.fontSize;
        transform.scaleX = target.scaleX;
        transform.scaleY = target.scaleY;
        transform.hasPrimaryColor = target.hasPrimaryColor;
        transform.primaryR = target.primaryR;
        transform.primaryG = target.primaryG;
        transform.primaryB = target.primaryB;
        transform.hasPrimaryAlpha = target.hasPrimaryAlpha;
        transform.primaryAlpha = target.primaryAlpha;
        transform.hasBackColor = target.hasBackColor;
        transform.backR = target.backR;
        transform.backG = target.backG;
        transform.backB = target.backB;
        transform.hasBackAlpha = target.hasBackAlpha;
        transform.backAlpha = target.backAlpha;
        if (transform.fontSize > 0.0f || transform.scaleX > 0.0f ||
            transform.scaleY > 0.0f || transform.hasPrimaryColor ||
            transform.hasPrimaryAlpha || transform.hasBackColor ||
            transform.hasBackAlpha) {
          out->transforms.push_back(transform);
        }
      }
      i = end + 1;
      continue;
    }

    auto parseClipTag = [&](size_t tagLen, bool inverse) {
      i += tagLen;
      if (i >= block.size() || block[i] != '(') return false;
      ++i;
      size_t end = block.find(')', i);
      if (end == std::string_view::npos) return false;
      std::vector<std::string> parts =
          splitAssTagArgs(block.substr(i, end - i));
      float x1 = 0.0f;
      float y1 = 0.0f;
      float x2 = 0.0f;
      float y2 = 0.0f;
      if (parts.size() == 4 && parseFloatAscii(parts[0], &x1) &&
          parseFloatAscii(parts[1], &y1) &&
          parseFloatAscii(parts[2], &x2) &&
          parseFloatAscii(parts[3], &y2)) {
        out->hasClip = true;
        out->inverseClip = inverse;
        out->clipX1 = std::min(x1, x2);
        out->clipY1 = std::min(y1, y2);
        out->clipX2 = std::max(x1, x2);
        out->clipY2 = std::max(y1, y2);
      }
      i = end + 1;
      return true;
    };

    if (startsWithAsciiNoCase(block, i, "iclip")) {
      if (parseClipTag(5, true)) continue;
    }

    if (startsWithAsciiNoCase(block, i, "clip")) {
      if (parseClipTag(4, false)) continue;
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
static void replaceAll(std::string* text, const std::string& from,
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

static void parseAssTextOverrides(std::string_view block, int* inOutPrimaryAlpha,
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
