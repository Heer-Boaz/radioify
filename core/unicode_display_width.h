#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

int unicodeDisplayWidth(char32_t codepoint);
bool utf8DecodeCodepoint(std::string_view text, std::size_t *offset,
                         char32_t *outCodepoint, std::size_t *outStart,
                         std::size_t *outEnd);
int utf8DisplayWidth(std::string_view text);
std::string utf8TakeDisplayWidth(std::string_view text, int width);
std::string utf8SliceDisplayWidth(std::string_view text, int startWidth,
                                  int width);

// Wraps a single logical line on ASCII word separators while measuring UTF-8
// terminal cells. An indivisible glyph wider than the requested width is kept
// intact so the function always makes progress without corrupting text.
std::vector<std::string> utf8WrapDisplayWidth(std::string_view text, int width);
