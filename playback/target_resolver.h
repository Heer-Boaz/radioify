#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "playback/target.h"

namespace playback_target_resolver {

std::optional<PlaybackTarget> resolvePlaybackTarget(
    const std::filesystem::path& path);

std::optional<PlaybackTarget> resolveDroppedTarget(
    const std::vector<std::filesystem::path>& files);

}  // namespace playback_target_resolver
