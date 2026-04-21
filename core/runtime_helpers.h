#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

std::string toUtf8String(const std::filesystem::path& path);
std::filesystem::path pathFromUtf8String(const std::string& value);
int64_t nowUs();
std::filesystem::path radioifyLaunchDir();
std::filesystem::path radioifyExecutableDir();
std::filesystem::path radioifyWritableDataDir();
std::filesystem::path radioifyCrashDumpDir();
std::filesystem::path radioifyLogPath();
std::vector<std::filesystem::path> radioifyResourceSearchRoots();

bool validateSupportedAudioInputFile(const std::filesystem::path& path,
                                     std::string* error);
void requireSupportedAudioInputFile(const std::filesystem::path& path);
