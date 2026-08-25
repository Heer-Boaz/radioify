#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace playback_video_transcript {

enum class VulkanDeviceClass {
  Integrated,
  Discrete,
};

// The index is in whisper.cpp's filtered GPU/IGPU device list, not in a
// backend-local list. Keeping that distinction here prevents a Vulkan device
// number from being passed through as an unrelated Whisper device number.
struct VulkanDeviceCandidate {
  int whisperGpuIndex = -1;
  VulkanDeviceClass deviceClass = VulkanDeviceClass::Integrated;
  size_t freeMemory = 0;
  size_t totalMemory = 0;
  std::string name;
  std::string description;
};

std::optional<VulkanDeviceCandidate> selectPreferredVulkanDevice(
    const std::vector<VulkanDeviceCandidate>& candidates);

}  // namespace playback_video_transcript
