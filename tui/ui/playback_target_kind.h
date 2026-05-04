#pragma once

#include "playback_target.h"

enum class PlaybackTargetKind {
  Audio,
  Image,
  Video,
};

PlaybackTargetKind classifyPlaybackTarget(const PlaybackTarget& target);
