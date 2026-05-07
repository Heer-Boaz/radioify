#pragma once

enum class PlayerState {
  Idle,
  Opening,
  Prefill,
  Priming,
  Playing,
  Paused,
  Seeking,
  Draining,
  Ended,
  Error,
  Closing,
};
