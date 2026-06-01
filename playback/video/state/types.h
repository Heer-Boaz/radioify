#pragma once

enum class PlayerState {
  Idle,
  Opening,
  Prefill,
  Priming,
  Playing,
  Paused,
  FrameStep,
  Seeking,
  Draining,
  Ended,
  Error,
  Closing,
};
