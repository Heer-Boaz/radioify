#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "playback/media_action_catalog.h"
#include "playback/media_processing_service.h"

namespace playback_media_processing {

struct ActionResult {
  bool accepted = false;
  std::string feedback;
};

// Small nullable facade copied into playback sessions. The bound service is
// borrowed and must outlive every Actions copy.
class Actions {
 public:
  Actions() = default;
  explicit Actions(Service& service) : service_(&service) {}

  std::optional<ActionResult> execute(
      playback_media_actions::Action action,
      const std::filesystem::path& sourceFile) const;
  void applySourceState(const std::filesystem::path& sourceFile,
                        playback_media_actions::Context& context) const;

 private:
  Service* service_ = nullptr;
};

}  // namespace playback_media_processing
