#include "playback/video/transcript/device_selection.h"

namespace playback_video_transcript {
namespace {

bool preferredOver(const VulkanDeviceCandidate& candidate,
                   const VulkanDeviceCandidate& current) {
  const bool candidateDiscrete =
      candidate.deviceClass == VulkanDeviceClass::Discrete;
  const bool currentDiscrete =
      current.deviceClass == VulkanDeviceClass::Discrete;
  if (candidateDiscrete != currentDiscrete) return candidateDiscrete;
  if (candidate.totalMemory != current.totalMemory) {
    return candidate.totalMemory > current.totalMemory;
  }
  if (candidate.freeMemory != current.freeMemory) {
    return candidate.freeMemory > current.freeMemory;
  }
  return candidate.whisperGpuIndex < current.whisperGpuIndex;
}

}  // namespace

std::optional<VulkanDeviceCandidate> selectPreferredVulkanDevice(
    const std::vector<VulkanDeviceCandidate>& candidates) {
  std::optional<VulkanDeviceCandidate> selected;
  for (const VulkanDeviceCandidate& candidate : candidates) {
    if (candidate.whisperGpuIndex < 0) continue;
    if (!selected || preferredOver(candidate, *selected)) {
      selected = candidate;
    }
  }
  return selected;
}

}  // namespace playback_video_transcript
