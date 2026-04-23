#pragma once

#include <filesystem>
#include <optional>
#include <vector>

#include "playback/session/playback_session_state.h"
#include "playback_target.h"

namespace playback_drop_route {

enum class DropSurface {
  Browser,
  AudioMiniPlayer,
  VideoPresentation,
};

enum class DroppedMediaKind {
  Audio,
  Video,
  Image,
};

struct DropRoute {
  PlaybackTarget target;
  DroppedMediaKind kind = DroppedMediaKind::Audio;
  bool closeAudioMiniPlayer = false;
  bool openAudioMiniPlayer = false;
  std::optional<PlaybackSessionContinuationState> videoContinuation;
};

std::optional<DropRoute> resolve(
    const std::vector<std::filesystem::path>& files, DropSurface surface,
    const PlaybackWindowPlacementState* sourcePlacement = nullptr,
    bool preferTextGridVideoMiniPlayer = true);

}  // namespace playback_drop_route
