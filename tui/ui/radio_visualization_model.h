#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "audio/radio_visualization.h"

namespace tui_radio_visualization {

struct Levels {
  RadioFilterMode mode = RadioFilterMode::Off;
  uint64_t generation = 0;
  uint32_t observed = 0;
  std::array<float, kRadioCircuitProbeCount> values{};

  bool has(RadioCircuitProbe probe) const {
    return (observed & radioCircuitProbeBit(probe)) != 0;
  }
};

enum class PlaybackPhase {
  Playing,
  Paused,
  Finished,
};

// One presentation update shared by every visible receiver surface.
struct Snapshot {
  RadioFilterMode requestedMode = RadioFilterMode::Off;
  PlaybackPhase playback = PlaybackPhase::Playing;
  std::optional<Levels> levels;
};

// Convert the independently measured physical quantities to visual intensity.
// References are display choices for the typical receiver, not watts.
Levels project(const RadioPlaybackObservation& observation);

enum class Status {
  SelectTypical,
  UnsupportedReceiver,
  Waiting,
  ResumeToObserve,
  Switching,
  NoCircuitObservation,
  Playing,
  Paused,
  Finished,
};

struct Presentation {
  Status status = Status::Waiting;
  bool showCircuit = false;
};

Presentation present(const Levels* levels, RadioFilterMode requestedMode,
                     PlaybackPhase playback);

}  // namespace tui_radio_visualization
