#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "playback/target.h"

namespace playback_target_resolver {

struct PlaybackTargetResolveOptions {
  bool includeImages = false;
};

std::optional<PlaybackTarget> resolvePathTarget(
    const std::filesystem::path& path,
    PlaybackTargetResolveOptions options = {});

std::optional<PlaybackTarget> resolveDroppedTarget(
    const std::vector<std::filesystem::path>& files);

}  // namespace playback_target_resolver
