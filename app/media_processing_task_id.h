#pragma once

#include <cstdint>

namespace media_processing {

// Stable identity of one accepted processing request. Zero is reserved for an
// absent identity so task IDs can cross presentation boundaries by value.
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

}  // namespace media_processing
