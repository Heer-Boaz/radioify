#pragma once

#include <optional>
#include <string>
#include <string_view>

std::optional<std::wstring> utf8ToWideStrict(std::string_view text);
std::wstring utf8ToWideLossy(std::string_view text);

std::optional<std::string> wideToUtf8Strict(std::wstring_view text);
std::string wideToUtf8Lossy(std::wstring_view text);

bool isValidUtf8(std::string_view text);
