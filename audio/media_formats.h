#pragma once

#include <filesystem>
#include <string>

bool isSupportedAudioExt(const std::filesystem::path& path);
bool isSupportedVideoExt(const std::filesystem::path& path);
bool isSupportedImageExt(const std::filesystem::path& path);
bool isSupportedMediaExt(const std::filesystem::path& path);
bool isMiniaudioExt(const std::filesystem::path& path);
bool isFlacExt(const std::filesystem::path& path);
bool isGmeExt(const std::filesystem::path& path);
bool isMidiExt(const std::filesystem::path& path);
bool isGsfExt(const std::filesystem::path& path);
bool isVgmExt(const std::filesystem::path& path);
bool isM4aExt(const std::filesystem::path& path);
bool isKssExt(const std::filesystem::path& path);
bool isPsfExt(const std::filesystem::path& path);
bool isOggExt(const std::filesystem::path& path);

std::string supportedAudioExtensionsText();
