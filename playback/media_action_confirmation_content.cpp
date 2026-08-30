#include "playback/media_action_confirmation_content.h"

#include <utility>

#include "playback/media_processing_actions.h"

namespace playback_media_confirmation {

Content cancellationContent(
    playback_media_processing::Operation operation,
    std::string sourceName) {
  Content content;
  content.title =
      std::string("Cancel ") +
      playback_media_processing::operationDisplayName(operation) + "?";
  content.text =
      operation ==
              playback_media_processing::Operation::AudioSeparationSetup
          ? std::vector<std::string>{
                "The optional NVIDIA component download will stop."}
          : std::vector<std::string>{"Progress on " + std::move(sourceName) +
                                     " will be lost."};
  content.primaryLabel = "Cancel task";
  content.secondaryLabel = "Keep running";
  return content;
}

Content audioSeparationSetupContent() {
  Content content;
  content.title = "Set up audio separation?";
  content.text = {
      "Radioify needs the optional NVIDIA TensorRT-RTX component for GPU "
      "audio separation.",
      "Windows will download and install this shared system component. "
      "Internet access is required and setup may take several minutes."};
  content.primaryLabel = "Set up";
  content.secondaryLabel = "Not now";
  return content;
}

}  // namespace playback_media_confirmation
