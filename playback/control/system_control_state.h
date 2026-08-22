#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "playback/target.h"

enum class PlaybackControlStatus : uint8_t {
  Stopped,
  Playing,
  Paused,
};

struct PlaybackControlState {
  explicit PlaybackControlState(PlaybackTarget playbackTarget,
                                bool video = false)
      : target(std::move(playbackTarget)), isVideo(video) {}

  PlaybackTarget target;
  bool isVideo = false;
  PlaybackControlStatus status = PlaybackControlStatus::Stopped;
  double positionSec = 0.0;
  std::optional<double> durationSec;
  bool canPlay = true;
  bool canPause = true;
  bool canStop = true;
  bool canPrevious = false;
  bool canNext = false;
};
