#pragma once

#include <filesystem>

#include "playback/media_action_catalog.h"

namespace playback_media_actions {

// Classifies a source once for every surface that projects the action catalog.
// Video wins for container extensions that can also carry audio-only media.
MediaKind mediaKindForSource(const std::filesystem::path& sourceFile);

}  // namespace playback_media_actions
