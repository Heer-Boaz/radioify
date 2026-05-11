#pragma once

#include <cstdint>
#include <filesystem>

enum class PlaybackControlStatus : uint8_t {
  Closed,
  Stopped,
  Playing,
  Paused,
};

struct PlaybackControlState {
  bool active = false;
  bool isVideo = false;
  std::filesystem::path file;
  int trackIndex = -1;
  PlaybackControlStatus status = PlaybackControlStatus::Closed;
  double positionSec = 0.0;
  double durationSec = -1.0;
  bool canPlay = true;
  bool canPause = true;
  bool canStop = true;
  bool canPrevious = false;
  bool canNext = false;
};
