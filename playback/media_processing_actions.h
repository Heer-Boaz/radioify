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
  std::optional<RequestError> error;
};

// Converts a typed application request result into consistent user feedback.
// Both playback-owned and application-owned media actions use this boundary.
ActionResult makeActionResult(playback_media_actions::Action action,
                              const std::filesystem::path& sourceFile,
                              const RequestResult& requestResult);

// Required command facade copied into playback surfaces. The application
// service is the long-lived owner and must outlive the surfaces it creates.
class Actions {
 public:
  explicit Actions(Service& service) : service_(service) {}

  std::optional<ActionResult> execute(
      playback_media_actions::Action action,
      const std::filesystem::path& sourceFile) const;
  playback_media_actions::Context contextForSource(
      const std::filesystem::path& sourceFile) const;

 private:
  Service& service_;
};

}  // namespace playback_media_processing
