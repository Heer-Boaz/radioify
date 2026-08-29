#pragma once

#include <cstdint>
#include <filesystem>

namespace playback_session {

enum class TransitionStage : std::uint8_t {
  Opening,
  Cancelling,
  Closing,
};

struct TransitionSnapshot {
  std::filesystem::path file;
  TransitionStage stage = TransitionStage::Opening;
  double activity = 0.0;
};

}  // namespace playback_session
