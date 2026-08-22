#include "utf8.h"

#include <cstdint>
#include <limits>

namespace {

constexpr char32_t kReplacementCharacter = 0xfffd;

bool isContinuation(unsigned char byte) {
  return (byte & 0xc0u) == 0x80u;
}

bool decodeCodePoint(std::string_view text, std::size_t offset,
                     char32_t* codePoint, std::size_t* length) {
  if (!codePoint || !length || offset >= text.size()) return false;
  const auto byte = [&](std::size_t index) {
    return static_cast<unsigned char>(text[offset + index]);
  };
  const unsigned char lead = byte(0);
  if (lead <= 0x7f) {
    *codePoint = lead;
    *length = 1;
    return true;
  }
  if (lead >= 0xc2 && lead <= 0xdf && offset + 1 < text.size() &&
      isContinuation(byte(1))) {
    *codePoint = static_cast<char32_t>((lead & 0x1fu) << 6) |
                 static_cast<char32_t>(byte(1) & 0x3fu);
    *length = 2;
    return true;
  }
  if (lead >= 0xe0 && lead <= 0xef && offset + 2 < text.size() &&
      isContinuation(byte(1)) && isContinuation(byte(2)) &&
      (lead != 0xe0 || byte(1) >= 0xa0) &&
      (lead != 0xed || byte(1) <= 0x9f)) {
    *codePoint = static_cast<char32_t>((lead & 0x0fu) << 12) |
                 static_cast<char32_t>((byte(1) & 0x3fu) << 6) |
                 static_cast<char32_t>(byte(2) & 0x3fu);
    *length = 3;
    return true;
  }
  if (lead >= 0xf0 && lead <= 0xf4 && offset + 3 < text.size() &&
      isContinuation(byte(1)) && isContinuation(byte(2)) &&
      isContinuation(byte(3)) &&
      (lead != 0xf0 || byte(1) >= 0x90) &&
      (lead != 0xf4 || byte(1) <= 0x8f)) {
    *codePoint = static_cast<char32_t>((lead & 0x07u) << 18) |
                 static_cast<char32_t>((byte(1) & 0x3fu) << 12) |
                 static_cast<char32_t>((byte(2) & 0x3fu) << 6) |
                 static_cast<char32_t>(byte(3) & 0x3fu);
    *length = 4;
    return true;
  }
  return false;
}

void appendWideCodePoint(std::wstring& output, char32_t codePoint) {
  if constexpr (sizeof(wchar_t) == 2) {
    if (codePoint <= 0xffff) {
      output.push_back(static_cast<wchar_t>(codePoint));
      return;
    }
    codePoint -= 0x10000;
    output.push_back(
        static_cast<wchar_t>(0xd800 + ((codePoint >> 10) & 0x3ff)));
    output.push_back(static_cast<wchar_t>(0xdc00 + (codePoint & 0x3ff)));
  } else {
    output.push_back(static_cast<wchar_t>(codePoint));
  }
}

template <bool ReplaceInvalid>
std::optional<std::wstring> decodeUtf8(std::string_view text) {
  std::wstring output;
  output.reserve(text.size());
  std::size_t offset = 0;
  while (offset < text.size()) {
    char32_t codePoint = 0;
    std::size_t length = 0;
    if (!decodeCodePoint(text, offset, &codePoint, &length)) {
      if constexpr (!ReplaceInvalid) return std::nullopt;
      appendWideCodePoint(output, kReplacementCharacter);
      ++offset;
      continue;
    }
    appendWideCodePoint(output, codePoint);
    offset += length;
  }
  return output;
}

bool nextWideCodePoint(std::wstring_view text, std::size_t* offset,
                       char32_t* codePoint) {
  if (!offset || !codePoint || *offset >= text.size()) return false;
  const char32_t first = static_cast<char32_t>(text[*offset]);
  ++*offset;
  if constexpr (sizeof(wchar_t) == 2) {
    if (first >= 0xd800 && first <= 0xdbff) {
      if (*offset >= text.size()) return false;
      const char32_t second = static_cast<char32_t>(text[*offset]);
      if (second < 0xdc00 || second > 0xdfff) return false;
      ++*offset;
      *codePoint = 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
      return true;
    }
    if (first >= 0xdc00 && first <= 0xdfff) return false;
  } else if (first > 0x10ffff || (first >= 0xd800 && first <= 0xdfff)) {
    return false;
  }
  *codePoint = first;
  return true;
}

void appendUtf8(std::string& output, char32_t codePoint) {
  if (codePoint <= 0x7f) {
    output.push_back(static_cast<char>(codePoint));
  } else if (codePoint <= 0x7ff) {
    output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
  } else if (codePoint <= 0xffff) {
    output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
  } else {
    output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
    output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
  }
}

template <bool ReplaceInvalid>
std::optional<std::string> encodeWide(std::wstring_view text) {
  std::string output;
  output.reserve(text.size());
  std::size_t offset = 0;
  while (offset < text.size()) {
    const std::size_t start = offset;
    char32_t codePoint = 0;
    if (!nextWideCodePoint(text, &offset, &codePoint)) {
      if constexpr (!ReplaceInvalid) return std::nullopt;
      offset = start + 1;
      codePoint = kReplacementCharacter;
    }
    appendUtf8(output, codePoint);
  }
  return output;
}

}  // namespace

std::optional<std::wstring> utf8ToWideStrict(std::string_view text) {
  return decodeUtf8<false>(text);
}

std::wstring utf8ToWideLossy(std::string_view text) {
  return *decodeUtf8<true>(text);
}

std::optional<std::string> wideToUtf8Strict(std::wstring_view text) {
  return encodeWide<false>(text);
}

std::string wideToUtf8Lossy(std::wstring_view text) {
  return *encodeWide<true>(text);
}

bool isValidUtf8(std::string_view text) {
  return utf8ToWideStrict(text).has_value();
}
