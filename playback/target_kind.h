#pragma once

#include "playback/target.h"

enum class PlaybackTargetKind {
  Audio,
  Image,
  Video,
};

PlaybackTargetKind classifyPlaybackTarget(const PlaybackTarget& target);
