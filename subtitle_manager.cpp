#include "subtitle_manager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
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
};

struct AssOverrideInfo {
  int alignment = -1;
  bool hasPosition = false;
  float posX = 0.0f;
  float posY = 0.0f;
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
};

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

    if (i + 2 <= block.size() && block.substr(i, 2) == "an") {
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

    if (i + 3 <= block.size() && block.substr(i, 3) == "pos") {
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

    if (i + 4 <= block.size() && block.substr(i, 4) == "move") {
      i += 4;
      if (i >= block.size() || block[i] != '(') continue;
      ++i;
      size_t end = block.find(')', i);
      if (end == std::string_view::npos) continue;
      std::string args = std::string(block.substr(i, end - i));
      size_t c1 = args.find(',');
      if (c1 != std::string::npos) {
        size_t c2 = args.find(',', c1 + 1);
        if (c2 != std::string::npos) {
          float x = 0.0f;
          float y = 0.0f;
          if (parseFloatAscii(args.substr(0, c1), &x) &&
              parseFloatAscii(args.substr(c1 + 1, c2 - c1 - 1), &y)) {
            out->hasPosition = true;
            out->posX = x;
            out->posY = y;
          }
        }
      }
      i = end + 1;
      continue;
    }

    if (i + 4 <= block.size() && block.substr(i, 4) == "fscx") {
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

    if (i + 4 <= block.size() && block.substr(i, 4) == "fscy") {
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

    if (i + 2 <= block.size() && block.substr(i, 2) == "fs") {
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

    if (i + 2 <= block.size() && block.substr(i, 2) == "fn") {
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
  bool inBrace = false;
  for (char ch : noHtml) {
    if (ch == '{') {
      inBrace = true;
      continue;
    }
    if (inBrace) {
      if (ch == '}') inBrace = false;
      continue;
    }
    noAss.push_back(ch);
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
      outCues->push_back(SubtitleCue{startUs, endUs, std::move(text)});
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
      outCues->push_back(SubtitleCue{startUs, endUs, std::move(text)});
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
      outCues->push_back(SubtitleCue{startUs, endUs, std::move(text)});
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
    outCues->push_back(SubtitleCue{startUs, endUs, std::move(text)});
  }

  return !outCues->empty();
}

bool splitCsvPrefix(const std::string& text, size_t fieldCount,
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

bool parseAssCues(const std::string& raw, std::vector<SubtitleCue>* outCues) {
  if (!outCues) return false;
  std::vector<std::string> lines = splitLinesNormalized(raw);
  if (lines.empty()) return false;

  enum class AssSection { Other, ScriptInfo, Styles, Events };
  AssSection section = AssSection::Other;

  int playResX = 0;
  int playResY = 0;
  float baseFontSize = 0.0f;
  std::unordered_map<std::string, AssStyleInfo> styles;

  std::vector<std::string> styleFormatFields;
  int styleNameIndex = -1;
  int styleAlignmentIndex = -1;
  int styleMarginLIndex = -1;
  int styleMarginRIndex = -1;
  int styleMarginVIndex = -1;
  int styleFontSizeIndex = -1;
  int styleFontNameIndex = -1;
  int styleBoldIndex = -1;
  int styleItalicIndex = -1;
  int styleUnderlineIndex = -1;
  int styleScaleXIndex = -1;
  int styleScaleYIndex = -1;

  std::vector<std::string> eventFormatFields;
  int startIndex = -1;
  int endIndex = -1;
  int textIndex = -1;
  int styleIndex = -1;
  int layerIndex = -1;
  int marginLIndex = -1;
  int marginRIndex = -1;
  int marginVIndex = -1;

  auto parseFormatFields = [](const std::string& payload,
                              std::vector<std::string>* outFields) {
    if (!outFields) return;
    outFields->clear();
    size_t start = 0;
    while (start <= payload.size()) {
      size_t comma = payload.find(',', start);
      std::string field =
          (comma == std::string::npos)
              ? payload.substr(start)
              : payload.substr(start, comma - start);
      outFields->push_back(trimAscii(field));
      if (comma == std::string::npos) break;
      start = comma + 1;
    }
  };

  auto findFieldIndex = [](const std::vector<std::string>& fields,
                           const char* wanted) -> int {
    if (!wanted) return -1;
    std::string target = toLowerAscii(std::string(wanted));
    for (size_t i = 0; i < fields.size(); ++i) {
      if (toLowerAscii(trimAscii(fields[i])) == target) {
        return static_cast<int>(i);
      }
    }
    return -1;
  };

  for (const std::string& rawLine : lines) {
    std::string line = trimAscii(rawLine);
    if (line.empty()) continue;

    if (line.front() == '[' && line.back() == ']') {
      std::string sectionName = toLowerAscii(line);
      if (sectionName == "[script info]") {
        section = AssSection::ScriptInfo;
      } else if (sectionName == "[v4+ styles]" || sectionName == "[v4 styles]") {
        section = AssSection::Styles;
      } else if (sectionName == "[events]") {
        section = AssSection::Events;
      } else {
        section = AssSection::Other;
      }
      continue;
    }

    if (section == AssSection::ScriptInfo) {
      size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      std::string key = toLowerAscii(trimAscii(line.substr(0, colon)));
      std::string value = trimAscii(line.substr(colon + 1));
      int parsedInt = 0;
      if (key == "playresx" && parseNonNegativeInt(value, &parsedInt)) {
        playResX = parsedInt;
      } else if (key == "playresy" && parseNonNegativeInt(value, &parsedInt)) {
        playResY = parsedInt;
      }
      continue;
    }

    if (section == AssSection::Styles) {
      const std::string formatPrefix = "Format:";
      const std::string stylePrefix = "Style:";
      if (line.size() >= formatPrefix.size() &&
          startsWith(toLowerAscii(line.substr(0, formatPrefix.size())),
                     toLowerAscii(formatPrefix))) {
        parseFormatFields(line.substr(formatPrefix.size()), &styleFormatFields);
        styleNameIndex = findFieldIndex(styleFormatFields, "name");
        styleAlignmentIndex = findFieldIndex(styleFormatFields, "alignment");
        styleMarginLIndex = findFieldIndex(styleFormatFields, "marginl");
        styleMarginRIndex = findFieldIndex(styleFormatFields, "marginr");
        styleMarginVIndex = findFieldIndex(styleFormatFields, "marginv");
        styleFontSizeIndex = findFieldIndex(styleFormatFields, "fontsize");
        styleFontNameIndex = findFieldIndex(styleFormatFields, "fontname");
        styleBoldIndex = findFieldIndex(styleFormatFields, "bold");
        styleItalicIndex = findFieldIndex(styleFormatFields, "italic");
        styleUnderlineIndex = findFieldIndex(styleFormatFields, "underline");
        styleScaleXIndex = findFieldIndex(styleFormatFields, "scalex");
        styleScaleYIndex = findFieldIndex(styleFormatFields, "scaley");
        continue;
      }
      if (line.size() < stylePrefix.size() ||
          !startsWith(toLowerAscii(line.substr(0, stylePrefix.size())),
                      toLowerAscii(stylePrefix)) ||
          styleFormatFields.empty() || styleNameIndex < 0) {
        continue;
      }

      std::vector<std::string> fields;
      if (!splitCsvPrefix(line.substr(stylePrefix.size()), styleFormatFields.size(),
                          &fields)) {
        continue;
      }
      if (styleNameIndex >= static_cast<int>(fields.size())) continue;
      std::string styleName = trimAscii(fields[static_cast<size_t>(styleNameIndex)]);
      if (styleName.empty()) continue;

      AssStyleInfo style;
      int parsedInt = 0;
      float parsedFloat = 0.0f;
      if (styleAlignmentIndex >= 0 &&
          styleAlignmentIndex < static_cast<int>(fields.size()) &&
          parseNonNegativeInt(fields[static_cast<size_t>(styleAlignmentIndex)],
                              &parsedInt)) {
        style.alignment = clampAssAlignment(parsedInt);
      }
      if (styleMarginLIndex >= 0 &&
          styleMarginLIndex < static_cast<int>(fields.size()) &&
          parseNonNegativeInt(fields[static_cast<size_t>(styleMarginLIndex)],
                              &parsedInt)) {
        style.marginL = parsedInt;
      }
      if (styleMarginRIndex >= 0 &&
          styleMarginRIndex < static_cast<int>(fields.size()) &&
          parseNonNegativeInt(fields[static_cast<size_t>(styleMarginRIndex)],
                              &parsedInt)) {
        style.marginR = parsedInt;
      }
      if (styleMarginVIndex >= 0 &&
          styleMarginVIndex < static_cast<int>(fields.size()) &&
          parseNonNegativeInt(fields[static_cast<size_t>(styleMarginVIndex)],
                              &parsedInt)) {
        style.marginV = parsedInt;
      }
      if (styleFontSizeIndex >= 0 &&
          styleFontSizeIndex < static_cast<int>(fields.size()) &&
          parseFloatAscii(fields[static_cast<size_t>(styleFontSizeIndex)],
                          &parsedFloat) &&
          parsedFloat > 0.0f) {
        style.fontSize = parsedFloat;
      }
      if (styleFontNameIndex >= 0 &&
          styleFontNameIndex < static_cast<int>(fields.size())) {
        style.fontName =
            trimAscii(fields[static_cast<size_t>(styleFontNameIndex)]);
      }
      if (styleBoldIndex >= 0 && styleBoldIndex < static_cast<int>(fields.size()) &&
          parseSignedInt(fields[static_cast<size_t>(styleBoldIndex)], &parsedInt)) {
        style.bold = parsedInt != 0;
      }
      if (styleItalicIndex >= 0 &&
          styleItalicIndex < static_cast<int>(fields.size()) &&
          parseSignedInt(fields[static_cast<size_t>(styleItalicIndex)], &parsedInt)) {
        style.italic = parsedInt != 0;
      }
      if (styleUnderlineIndex >= 0 &&
          styleUnderlineIndex < static_cast<int>(fields.size()) &&
          parseSignedInt(fields[static_cast<size_t>(styleUnderlineIndex)],
                         &parsedInt)) {
        style.underline = parsedInt != 0;
      }
      if (styleScaleXIndex >= 0 &&
          styleScaleXIndex < static_cast<int>(fields.size()) &&
          parseFloatAscii(fields[static_cast<size_t>(styleScaleXIndex)],
                          &parsedFloat) &&
          parsedFloat > 0.0f) {
        style.scaleX = std::clamp(parsedFloat * 0.01f, 0.40f, 3.5f);
      }
      if (styleScaleYIndex >= 0 &&
          styleScaleYIndex < static_cast<int>(fields.size()) &&
          parseFloatAscii(fields[static_cast<size_t>(styleScaleYIndex)],
                          &parsedFloat) &&
          parsedFloat > 0.0f) {
        style.scaleY = std::clamp(parsedFloat * 0.01f, 0.40f, 3.5f);
      }
      std::string nameLower = toLowerAscii(styleName);
      styles[nameLower] = style;
      if (baseFontSize <= 0.0f && style.fontSize > 0.0f) {
        if (nameLower == "default") {
          baseFontSize = style.fontSize;
        } else if (baseFontSize <= 0.0f) {
          baseFontSize = style.fontSize;
        }
      }
      continue;
    }

    if (section != AssSection::Events) continue;

    const std::string formatPrefix = "Format:";
    const std::string dialoguePrefix = "Dialogue:";
    if (line.size() >= formatPrefix.size() &&
        startsWith(toLowerAscii(line.substr(0, formatPrefix.size())),
                   toLowerAscii(formatPrefix))) {
      parseFormatFields(line.substr(formatPrefix.size()), &eventFormatFields);
      startIndex = findFieldIndex(eventFormatFields, "start");
      endIndex = findFieldIndex(eventFormatFields, "end");
      textIndex = findFieldIndex(eventFormatFields, "text");
      styleIndex = findFieldIndex(eventFormatFields, "style");
      layerIndex = findFieldIndex(eventFormatFields, "layer");
      marginLIndex = findFieldIndex(eventFormatFields, "marginl");
      marginRIndex = findFieldIndex(eventFormatFields, "marginr");
      marginVIndex = findFieldIndex(eventFormatFields, "marginv");
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

    std::string text = rawText;
    replaceAll(&text, "\\N", "\n");
    replaceAll(&text, "\\n", "\n");
    text = stripSubtitleMarkup(text);
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
    if (overrides.hasPosition) {
      cue.hasPosition = true;
      cue.posXNorm =
          std::clamp(overrides.posX / static_cast<float>(scriptW), 0.0f, 1.0f);
      cue.posYNorm =
          std::clamp(overrides.posY / static_cast<float>(scriptH), 0.0f, 1.0f);
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
    return tryParser(payload, &parseSrtCues, outCues) ||
           tryParser(payload, &parseVttCues, outCues) ||
           tryParser(payload, &parseAssCues, outCues) ||
           tryParser(payload, &parseSbvCues, outCues) ||
           tryParser(payload, &parseMicroDvdCues, outCues);
  };

  if (!outTrack) return false;
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return false;
  std::string rawBytes((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
  if (rawBytes.empty()) return false;
  std::string raw = decodeSubtitleText(rawBytes);
  if (raw.empty()) return false;

  std::string ext = toLowerAscii(path.extension().string());
  bool ok = false;
  if (ext == ".srt") {
    ok = tryParser(raw, &parseSrtCues, &outTrack->cues);
  } else if (ext == ".vtt") {
    ok = tryParser(raw, &parseVttCues, &outTrack->cues);
  } else if (ext == ".ass" || ext == ".ssa") {
    ok = tryParser(raw, &parseAssCues, &outTrack->cues);
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
    dir = std::filesystem::current_path(ec);
    if (ec) return {};
  }
  std::string baseStem = videoPath.stem().string();
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
    std::string key = toLowerAscii(searchDir.lexically_normal().string());
    for (const auto& existing : searchDirs) {
      if (toLowerAscii(existing.path.lexically_normal().string()) == key) {
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
    std::string nameLower = toLowerAscii(entry.path().filename().string());
    if (isSubtitleDirectoryName(nameLower)) {
      addSearchDir(entry.path(), 2);
    }
  }
  if (ec) ec.clear();

  auto maybeAddCandidate = [&](const std::filesystem::path& candidate) {
    std::string extLower = toLowerAscii(candidate.extension().string());
    if (!isSubtitleSidecarExtension(extLower)) return;
    int rank = rankForStem(candidate.stem().string());
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
              return toLowerAscii(a.second.string()) <
                     toLowerAscii(b.second.string());
            });

  ranked.erase(std::unique(ranked.begin(), ranked.end(),
                           [](const auto& a, const auto& b) {
                             return toLowerAscii(a.second.string()) ==
                                    toLowerAscii(b.second.string());
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
  std::string stem = path.stem().string();
  std::string label;
  std::string prefix = baseStem + ".";
  if (stem.rfind(prefix, 0) == 0 && stem.size() > prefix.size()) {
    label = stem.substr(prefix.size());
  } else {
    label = path.extension().string();
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

bool parseAssPacketFields(const std::string& line, bool tenFieldDialogueFormat,
                          int* outLayer, int* outMarginL, int* outMarginR,
                          int* outMarginV, std::string* outText) {
  if (!outText) return false;
  std::vector<std::string> fields;
  const size_t fieldCount = tenFieldDialogueFormat ? 10u : 9u;
  if (!splitCsvPrefix(line, fieldCount, &fields) || fields.size() < fieldCount) {
    return false;
  }

  const int layerIndex = tenFieldDialogueFormat ? 0 : 1;
  const int marginLIndex = tenFieldDialogueFormat ? 5 : 4;
  const int marginRIndex = tenFieldDialogueFormat ? 6 : 5;
  const int marginVIndex = tenFieldDialogueFormat ? 7 : 6;
  const int textIndex = tenFieldDialogueFormat ? 9 : 8;
  int parsedInt = 0;

  if (tenFieldDialogueFormat) {
    if (fields.size() < 3) return false;
    int64_t startUs = 0;
    int64_t endUs = 0;
    if (!parseTimecodeUs(fields[1], &startUs) ||
        !parseTimecodeUs(fields[2], &endUs)) {
      return false;
    }
  }

  const bool validLayer =
      (layerIndex >= 0 && layerIndex < static_cast<int>(fields.size()) &&
       parseSignedInt(fields[static_cast<size_t>(layerIndex)], &parsedInt));
  if (!validLayer) return false;
  if (outLayer) {
    *outLayer = std::max(0, parsedInt);
  }
  if (outMarginL && marginLIndex >= 0 &&
      marginLIndex < static_cast<int>(fields.size()) &&
      parseNonNegativeInt(fields[static_cast<size_t>(marginLIndex)], &parsedInt)) {
    *outMarginL = parsedInt;
  }
  if (outMarginR && marginRIndex >= 0 &&
      marginRIndex < static_cast<int>(fields.size()) &&
      parseNonNegativeInt(fields[static_cast<size_t>(marginRIndex)], &parsedInt)) {
    *outMarginR = parsedInt;
  }
  if (outMarginV && marginVIndex >= 0 &&
      marginVIndex < static_cast<int>(fields.size()) &&
      parseNonNegativeInt(fields[static_cast<size_t>(marginVIndex)], &parsedInt)) {
    *outMarginV = parsedInt;
  }

  if (textIndex < 0 || textIndex >= static_cast<int>(fields.size())) return false;
  *outText = fields[static_cast<size_t>(textIndex)];
  return true;
}

bool packetSubtitleCue(AVCodecID codecId, const AVPacket& pkt, SubtitleCue* outCue) {
  if (!outCue || !pkt.data || pkt.size <= 0) return false;
  std::string payload(reinterpret_cast<const char*>(pkt.data),
                      reinterpret_cast<const char*>(pkt.data + pkt.size));

  SubtitleCue cue;
  if (codecId == AV_CODEC_ID_ASS || codecId == AV_CODEC_ID_SSA) {
    std::string line = trimAscii(payload);
    std::string lower = toLowerAscii(line);
    bool hasDialoguePrefix = startsWith(lower, "dialogue:");
    if (hasDialoguePrefix) {
      size_t colon = line.find(':');
      if (colon != std::string::npos) {
        line = trimAscii(line.substr(colon + 1));
      }
    }

    int marginL = 0;
    int marginR = 0;
    int marginV = 0;
    bool parsedAssFields = false;
    if (hasDialoguePrefix) {
      parsedAssFields = parseAssPacketFields(
          line, true, &cue.layer, &marginL, &marginR, &marginV, &payload);
      if (!parsedAssFields) {
        parsedAssFields = parseAssPacketFields(
            line, false, &cue.layer, &marginL, &marginR, &marginV, &payload);
      }
    } else {
      parsedAssFields = parseAssPacketFields(
          line, false, &cue.layer, &marginL, &marginR, &marginV, &payload);
      if (!parsedAssFields) {
        parsedAssFields = parseAssPacketFields(
            line, true, &cue.layer, &marginL, &marginR, &marginV, &payload);
      }
    }
    if (!parsedAssFields) return false;

    cue.marginLNorm =
        std::clamp(static_cast<float>(std::max(0, marginL)) / 384.0f, 0.0f, 0.7f);
    cue.marginRNorm =
        std::clamp(static_cast<float>(std::max(0, marginR)) / 384.0f, 0.0f, 0.7f);
    cue.marginVNorm =
        std::clamp(static_cast<float>(std::max(0, marginV)) / 288.0f, 0.0f, 0.7f);

    AssOverrideInfo overrides;
    parseAssOverridesFromText(payload, &overrides);
    if (overrides.alignment >= 1) {
      cue.alignment = clampAssAlignment(overrides.alignment);
    }
    if (overrides.hasPosition) {
      cue.hasPosition = true;
      cue.posXNorm = std::clamp(overrides.posX / 384.0f, 0.0f, 1.0f);
      cue.posYNorm = std::clamp(overrides.posY / 288.0f, 0.0f, 1.0f);
    }
    if (overrides.fontSize > 0.0f) {
      cue.sizeScale = std::clamp(overrides.fontSize / 48.0f, 0.45f, 2.5f);
    }
    if (overrides.scaleX > 0.0f) {
      cue.scaleX = std::clamp(overrides.scaleX, 0.40f, 3.5f);
    }
    if (overrides.scaleY > 0.0f) {
      cue.scaleY = std::clamp(overrides.scaleY, 0.40f, 3.5f);
      cue.sizeScale = std::clamp(cue.sizeScale * cue.scaleY, 0.35f, 3.5f);
    }
    if (overrides.hasFontName) {
      cue.fontName = overrides.fontName;
    }
    if (overrides.hasBold) {
      cue.bold = overrides.bold;
    }
    if (overrides.hasItalic) {
      cue.italic = overrides.italic;
    }
    if (overrides.hasUnderline) {
      cue.underline = overrides.underline;
    }

  }
  replaceAll(&payload, "\\N", "\n");
  replaceAll(&payload, "\\n", "\n");
  cue.text = stripSubtitleMarkup(payload);

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
    SubtitleTrack track;
  };
  std::vector<EmbeddedTrackInfo> refs;

  for (unsigned i = 0; i < fmt->nb_streams; ++i) {
    AVStream* stream = fmt->streams[i];
    if (!stream || !stream->codecpar) continue;
    if (stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE) continue;
    EmbeddedTrackInfo info;
    info.streamIndex = static_cast<int>(i);
    info.timeBase = stream->time_base;
    info.codecId = stream->codecpar->codec_id;
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
      if (!packetSubtitleCue(ref.codecId, *pkt, &cue)) break;
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

  const std::string baseStem = videoPath.stem().string();
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
