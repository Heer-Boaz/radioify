#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

std::string toUtf8String(const std::filesystem::path& path);
int64_t nowUs();

bool validateSupportedAudioInputFile(const std::filesystem::path& path,
                                     std::string* error);
void requireSupportedAudioInputFile(const std::filesystem::path& path);
