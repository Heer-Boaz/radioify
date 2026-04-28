#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <string>
#include <string_view>

std::wstring parentPathOf(std::wstring_view path);
std::wstring joinPath(std::wstring_view parent, std::wstring_view child);
bool isExistingFile(const std::wstring& path) noexcept;
std::wstring extensionOfPath(std::wstring_view path);
std::wstring filenameStemOfPath(std::wstring_view path);
