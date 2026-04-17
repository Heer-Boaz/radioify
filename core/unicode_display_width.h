#pragma once

#include <string>
#include <string_view>

int unicodeDisplayWidth(char32_t codepoint);
bool utf8DecodeCodepoint(std::string_view text, size_t* offset,
                         char32_t* outCodepoint, size_t* outStart,
                         size_t* outEnd);
int utf8DisplayWidth(std::string_view text);
std::string utf8TakeDisplayWidth(std::string_view text, int width);
std::string utf8SliceDisplayWidth(std::string_view text, int startWidth,
                                  int width);
