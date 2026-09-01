#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace playback_video_chapters {

// Parses only the explicit device table emitted by the pinned
// llama-mtmd-cli --list-devices protocol. Backend diagnostic lines are not
// device identifiers. On hybrid systems the device with the most reported
// free memory is selected instead of relying on driver enumeration order.
std::optional<std::string> parseVulkanDeviceList(std::string_view output);

// Requires both the multimodal projector backend and every language-model
// layer to be on Vulkan. Merely mentioning GPU offload is not sufficient.
bool confirmsGpuOnlyInference(std::string_view output);

}  // namespace playback_video_chapters
