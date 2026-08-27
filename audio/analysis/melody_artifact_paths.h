#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

std::filesystem::path defaultMelodyArtifactPath(
    const std::filesystem::path& input);
std::filesystem::path melodyArtifactPathForTrack(
    const std::filesystem::path& input, std::uint32_t trackIndex);
std::filesystem::path resolveMelodyArtifactPath(
    const std::filesystem::path& input, const std::string& outputArgument);
