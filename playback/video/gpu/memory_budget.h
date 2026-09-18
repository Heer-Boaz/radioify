#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace playback_video_gpu {

struct MemoryBudget {
  uint64_t totalBytes = 0;
  uint64_t freeBytes = 0;
};

// Driver-wide, not process-local. PCI identity must match the actual inference
// adapter. Absence is unsupported telemetry, not zero free memory.
std::optional<MemoryBudget> queryMemoryBudget(std::string_view pciBusId);

}  // namespace playback_video_gpu
