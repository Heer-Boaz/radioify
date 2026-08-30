#pragma once

#include <cstdint>

namespace audio_playback {

// Distinguishes failures that leave the current endpoint intact from failures
// after replacement has begun. Callers use this to keep transport ownership
// aligned with the endpoint that is actually still audible.
enum class FileStartResult : std::uint8_t {
  Started,
  RejectedPreservingPlayback,
  FailedAfterReplacingPlayback,
};

constexpr bool fileStartSucceeded(FileStartResult result) noexcept {
  return result == FileStartResult::Started;
}

}  // namespace audio_playback
