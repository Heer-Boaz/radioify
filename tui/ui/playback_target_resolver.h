#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "browser_model.h"
#include "playback_target.h"

namespace playback_target_resolver {

struct PlaybackTargetResolveOptions {
  bool includeImages = false;
};

std::optional<PlaybackTarget>
resolvePathTarget(const std::filesystem::path& path,
                  PlaybackTargetResolveOptions options = {});

std::optional<PlaybackTarget>
resolveEntryTarget(const FileEntry& entry,
                   PlaybackTargetResolveOptions options = {});

std::optional<PlaybackTarget>
resolveDroppedTarget(const std::vector<std::filesystem::path>& files);

bool isPlayableEntry(const FileEntry& entry,
                     PlaybackTargetResolveOptions options = {});

}  // namespace playback_target_resolver
