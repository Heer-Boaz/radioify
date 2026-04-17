#include "unicode_display_width.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace {

struct Interval {
  char32_t first;
  char32_t last;
};

template <size_t N>
bool bisearch(char32_t codepoint, const std::array<Interval, N>& table) {
  size_t low = 0;
  size_t high = table.size();
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (codepoint > table[mid].last) {
      low = mid + 1;
    } else {
      high = mid;
    }
  }
  return low < table.size() && codepoint >= table[low].first &&
         codepoint <= table[low].last;
}

constexpr std::array<Interval, 125> kCombining = {{
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF},
    {0x05C1, 0x05C2}, {0x05C4, 0x05C5}, {0x05C7, 0x05C7}, {0x0600, 0x0605},
    {0x0610, 0x061A}, {0x061C, 0x061C}, {0x064B, 0x065F}, {0x0670, 0x0670},
    {0x06D6, 0x06DD}, {0x06DF, 0x06E4}, {0x06E7, 0x06E8}, {0x06EA, 0x06ED},
    {0x070F, 0x070F}, {0x0711, 0x0711}, {0x0730, 0x074A}, {0x07A6, 0x07B0},
    {0x07EB, 0x07F3}, {0x07FD, 0x07FD}, {0x0816, 0x0819}, {0x081B, 0x0823},
    {0x0825, 0x0827}, {0x0829, 0x082D}, {0x0859, 0x085B}, {0x0890, 0x0891},
    {0x0898, 0x089F}, {0x08CA, 0x0902}, {0x093A, 0x093A}, {0x093C, 0x093C},
    {0x0941, 0x0948}, {0x094D, 0x094D}, {0x0951, 0x0957}, {0x0962, 0x0963},
    {0x0981, 0x0981}, {0x09BC, 0x09BC}, {0x09C1, 0x09C4}, {0x09CD, 0x09CD},
    {0x09E2, 0x09E3}, {0x09FE, 0x09FE}, {0x0A01, 0x0A02}, {0x0A3C, 0x0A3C},
    {0x0A41, 0x0A42}, {0x0A47, 0x0A48}, {0x0A4B, 0x0A4D}, {0x0A51, 0x0A51},
    {0x0A70, 0x0A71}, {0x0A75, 0x0A75}, {0x0A81, 0x0A82}, {0x0ABC, 0x0ABC},
    {0x0AC1, 0x0AC5}, {0x0AC7, 0x0AC8}, {0x0ACD, 0x0ACD}, {0x0AE2, 0x0AE3},
    {0x0AFA, 0x0AFF}, {0x0B01, 0x0B01}, {0x0B3C, 0x0B3C}, {0x0B3F, 0x0B3F},
    {0x0B41, 0x0B44}, {0x0B4D, 0x0B4D}, {0x0B55, 0x0B56}, {0x0B62, 0x0B63},
    {0x0B82, 0x0B82}, {0x0BC0, 0x0BC0}, {0x0BCD, 0x0BCD}, {0x0C00, 0x0C00},
    {0x0C04, 0x0C04}, {0x0C3C, 0x0C3C}, {0x0C3E, 0x0C40}, {0x0C46, 0x0C48},
    {0x0C4A, 0x0C4D}, {0x0C55, 0x0C56}, {0x0C62, 0x0C63}, {0x0C81, 0x0C81},
    {0x0CBC, 0x0CBC}, {0x0CBF, 0x0CBF}, {0x0CC6, 0x0CC6}, {0x0CCC, 0x0CCD},
    {0x0CE2, 0x0CE3}, {0x0D00, 0x0D01}, {0x0D3B, 0x0D3C}, {0x0D41, 0x0D44},
    {0x0D4D, 0x0D4D}, {0x0D62, 0x0D63}, {0x0D81, 0x0D81}, {0x0DCA, 0x0DCA},
    {0x0DD2, 0x0DD4}, {0x0DD6, 0x0DD6}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A},
    {0x0E47, 0x0E4E}, {0x0EB1, 0x0EB1}, {0x0EB4, 0x0EBC}, {0x0EC8, 0x0ECD},
    {0x0F18, 0x0F19}, {0x0F35, 0x0F35}, {0x0F37, 0x0F37}, {0x0F39, 0x0F39},
    {0x0F71, 0x0F7E}, {0x0F80, 0x0F84}, {0x0F86, 0x0F87}, {0x0F8D, 0x0F97},
    {0x0F99, 0x0FBC}, {0x0FC6, 0x0FC6}, {0x102D, 0x1030}, {0x1032, 0x1037},
    {0x1039, 0x103A}, {0x103D, 0x103E}, {0x1058, 0x1059}, {0x105E, 0x1060},
    {0x1071, 0x1074}, {0x1082, 0x1082}, {0x1085, 0x1086}, {0x108D, 0x108D},
    {0x109D, 0x109D}, {0x135D, 0x135F}, {0x200B, 0x200F}, {0x202A, 0x202E},
    {0x2060, 0x2064}, {0x2066, 0x206F}, {0x20D0, 0x20F0}, {0xFE00, 0xFE0F},
    {0xFE20, 0xFE2F},
}};

bool isCombining(char32_t codepoint) {
  return bisearch(codepoint, kCombining);
}

bool decodeUtf8(std::string_view text, size_t* offset, char32_t* outCodepoint,
                size_t* outStart, size_t* outEnd) {
  if (!offset || *offset >= text.size()) return false;
  size_t start = *offset;
  unsigned char lead = static_cast<unsigned char>(text[start]);
  char32_t codepoint = 0;
  size_t length = 1;
  if (lead < 0x80) {
    codepoint = lead;
  } else if ((lead & 0xE0) == 0xC0 && start + 1 < text.size()) {
    codepoint = (lead & 0x1F) << 6;
    codepoint |= static_cast<unsigned char>(text[start + 1]) & 0x3F;
    length = 2;
  } else if ((lead & 0xF0) == 0xE0 && start + 2 < text.size()) {
    codepoint = (lead & 0x0F) << 12;
    codepoint |=
        (static_cast<unsigned char>(text[start + 1]) & 0x3F) << 6;
    codepoint |= static_cast<unsigned char>(text[start + 2]) & 0x3F;
    length = 3;
  } else if ((lead & 0xF8) == 0xF0 && start + 3 < text.size()) {
    codepoint = (lead & 0x07) << 18;
    codepoint |=
        (static_cast<unsigned char>(text[start + 1]) & 0x3F) << 12;
    codepoint |=
        (static_cast<unsigned char>(text[start + 2]) & 0x3F) << 6;
    codepoint |= static_cast<unsigned char>(text[start + 3]) & 0x3F;
    length = 4;
  } else {
    codepoint = 0xFFFD;
  }

  *offset = start + length;
  if (outCodepoint) *outCodepoint = codepoint;
  if (outStart) *outStart = start;
  if (outEnd) *outEnd = start + length;
  return true;
}

}  // namespace

int unicodeDisplayWidth(char32_t codepoint) {
  if (codepoint == 0) return 0;
  if (codepoint < 32 || (codepoint >= 0x7F && codepoint < 0xA0)) return 0;
  if (isCombining(codepoint)) return 0;

  if (codepoint >= 0x1100 &&
      (codepoint <= 0x115F || codepoint == 0x2329 || codepoint == 0x232A ||
       (codepoint >= 0x2E80 && codepoint <= 0xA4CF &&
        codepoint != 0x303F) ||
       (codepoint >= 0xAC00 && codepoint <= 0xD7A3) ||
       (codepoint >= 0xF900 && codepoint <= 0xFAFF) ||
       (codepoint >= 0xFE10 && codepoint <= 0xFE19) ||
       (codepoint >= 0xFE30 && codepoint <= 0xFE6F) ||
       (codepoint >= 0xFF00 && codepoint <= 0xFF60) ||
       (codepoint >= 0xFFE0 && codepoint <= 0xFFE6) ||
       (codepoint >= 0x1F300 && codepoint <= 0x1FAFF) ||
       (codepoint >= 0x20000 && codepoint <= 0x2FFFD) ||
       (codepoint >= 0x30000 && codepoint <= 0x3FFFD))) {
    return 2;
  }

  return 1;
}

bool utf8DecodeCodepoint(std::string_view text, size_t* offset,
                         char32_t* outCodepoint, size_t* outStart,
                         size_t* outEnd) {
  return decodeUtf8(text, offset, outCodepoint, outStart, outEnd);
}

int utf8DisplayWidth(std::string_view text) {
  int width = 0;
  size_t offset = 0;
  char32_t codepoint = 0;
  while (decodeUtf8(text, &offset, &codepoint, nullptr, nullptr)) {
    width += unicodeDisplayWidth(codepoint);
  }
  return width;
}

std::string utf8TakeDisplayWidth(std::string_view text, int width) {
  if (width <= 0) return "";
  size_t offset = 0;
  size_t end = 0;
  int columns = 0;
  char32_t codepoint = 0;
  size_t startByte = 0;
  size_t endByte = 0;
  while (decodeUtf8(text, &offset, &codepoint, &startByte, &endByte)) {
    int glyphWidth = unicodeDisplayWidth(codepoint);
    if (glyphWidth > 0 && columns + glyphWidth > width) {
      break;
    }
    end = endByte;
    columns += glyphWidth;
  }
  return std::string(text.substr(0, end));
}

std::string utf8SliceDisplayWidth(std::string_view text, int startWidth,
                                  int width) {
  if (width <= 0) return "";
  const int endWidth = startWidth + width;
  size_t offset = 0;
  size_t sliceStart = std::string_view::npos;
  size_t sliceEnd = std::string_view::npos;
  int columns = 0;
  char32_t codepoint = 0;
  size_t startByte = 0;
  size_t endByte = 0;
  while (decodeUtf8(text, &offset, &codepoint, &startByte, &endByte)) {
    int glyphWidth = unicodeDisplayWidth(codepoint);
    int nextColumns = columns + glyphWidth;
    bool overlaps = false;
    if (glyphWidth == 0) {
      overlaps = columns >= startWidth && columns < endWidth;
    } else {
      overlaps = nextColumns > startWidth && columns < endWidth;
    }
    if (overlaps) {
      if (sliceStart == std::string_view::npos) {
        sliceStart = startByte;
      }
      sliceEnd = endByte;
    } else if (sliceStart != std::string_view::npos && columns >= endWidth) {
      break;
    }
    columns = nextColumns;
  }
  if (sliceStart == std::string_view::npos || sliceEnd == std::string_view::npos) {
    return "";
  }
  return std::string(text.substr(sliceStart, sliceEnd - sliceStart));
}
