#pragma once

#include <array>
#include <cstdint>

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
};

struct Presentation {
  Status status = Status::Waiting;
  bool showCircuit = false;
};

Presentation present(const Levels* levels, RadioFilterMode requestedMode,
                     bool paused);

}  // namespace tui_radio_visualization
