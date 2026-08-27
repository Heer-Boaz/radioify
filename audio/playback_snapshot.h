#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

struct AudioPlaybackSource {
  std::filesystem::path file;
  std::optional<int> trackIndex;
};

// Immutable projection of the process-wide audio engine for one presentation
// update. Consumers use one snapshot instead of assembling a mixed state from
// independently timed getters.
struct AudioPlaybackSnapshot {
  std::optional<AudioPlaybackSource> source;
  bool ready = false;
  bool seeking = false;
  bool paused = false;
  bool finished = false;
  bool holding = false;
  bool radioEnabled = false;
  bool hz50Enabled = false;
  bool supports50HzToggle = false;
  double positionSec = 0.0;
  double durationSec = -1.0;
  double seekTargetSec = -1.0;
  float volume = 0.0f;
  float unclippedOutputPeak = 0.0f;
  std::string_view radioFilterLabel = "Radio: Off";
};
