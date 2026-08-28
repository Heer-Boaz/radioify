#pragma once

#include <cstdint>

namespace playback_media_processing {

// Stable identity of one accepted processing request. Zero is reserved for an
// absent identity so the application owner can safely carry task identity
// through playback and browser presentation boundaries by value.
struct TaskId {
  std::uint64_t value = 0;

  constexpr explicit operator bool() const { return value != 0; }
  friend constexpr bool operator==(TaskId left, TaskId right) {
    return left.value == right.value;
  }
  friend constexpr bool operator!=(TaskId left, TaskId right) {
    return !(left == right);
  }
};

}  // namespace playback_media_processing
