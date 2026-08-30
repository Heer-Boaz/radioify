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

inline constexpr const char* operationDisplayName(Operation operation) {
  switch (operation) {
    case Operation::MelodyAnalysis:
      return "melody analysis";
    case Operation::LoopSplit:
      return "loop split";
    case Operation::SubtitleGeneration:
      return "subtitle generation";
    case Operation::AudioSeparationSetup:
      return "audio separation setup";
    case Operation::AudioSeparation:
      return "audio separation";
    case Operation::AudioExport:
      return "audio export";
    case Operation::TranscriptTextExport:
      return "transcript export";
  }
  return "media processing";
}

// Stable intent shared by browser and playback surfaces. Presentation layers
// may ask for confirmation, but only the application task owner decides
// whether this exact task identity, operation and source are still active.
struct CancellationRequest {
  TaskId taskId;
  Operation operation = Operation::AudioExport;
  std::filesystem::path sourceFile;
};

struct AudioSeparationSetupRequest {
  std::filesystem::path sourceFile;
};

bool isCancellationAction(playback_media_actions::Action action);
std::optional<CancellationRequest> prepareCancellation(
    playback_media_actions::Action action,
    const std::filesystem::path& sourceFile, const SourceState& state);
std::optional<AudioSeparationSetupRequest> prepareAudioSeparationSetup(
    playback_media_actions::Action action,
    const std::filesystem::path& sourceFile, const SourceState& state);

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
  std::optional<CancellationRequest> prepareCancellation(
      playback_media_actions::Action action,
      const std::filesystem::path& sourceFile) const;
  std::optional<CancellationRequest> prepareCancellation(
      const Activity& activity) const;
  std::optional<AudioSeparationSetupRequest> prepareAudioSeparationSetup(
      playback_media_actions::Action action,
      const std::filesystem::path& sourceFile) const;
  ActionResult confirmCancellation(
      const CancellationRequest& request) const;
  ActionResult confirmAudioSeparationSetup(
      const AudioSeparationSetupRequest& request) const;
  playback_media_actions::Context contextForSource(
      const std::filesystem::path& sourceFile) const;

 private:
  Service& service_;
};

}  // namespace playback_media_processing
