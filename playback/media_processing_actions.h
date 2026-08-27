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

// Required command facade copied into playback surfaces. The application
// service is the long-lived owner and must outlive the surfaces it creates.
class Actions {
 public:
  explicit Actions(Service& service) : service_(service) {}

  std::optional<ActionResult> execute(
      playback_media_actions::Action action,
      const std::filesystem::path& sourceFile) const;
  void applySourceState(const std::filesystem::path& sourceFile,
                        playback_media_actions::Context& context) const;

 private:
  Service& service_;
};

}  // namespace playback_media_processing
