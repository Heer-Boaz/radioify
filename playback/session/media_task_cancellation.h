#pragma once

#include <optional>
#include <string>

#include "playback/media_processing_actions.h"

namespace playback_session {

enum class MediaTaskCancellationChoice {
  CancelTask,
  KeepRunning,
};

struct MediaTaskCancellationPrompt {
  playback_media_processing::CancellationRequest request;
  MediaTaskCancellationChoice selected =
      MediaTaskCancellationChoice::KeepRunning;
};

struct MediaTaskCancellationActivation {
  playback_media_processing::CancellationRequest request;
  bool cancelTask = false;
};

std::string mediaTaskCancellationTitle(
    playback_media_processing::Operation operation);
std::string mediaTaskCancellationSourceName(
    const MediaTaskCancellationPrompt& prompt);

// Session-owned modal state for playback surfaces. It owns only the pending
// decision; the application coordinator remains the authority that validates
// and cancels the stable task identity after confirmation.
class MediaTaskCancellationPromptState {
 public:
  bool open(playback_media_processing::CancellationRequest request);
  bool moveSelection(int direction);
  std::optional<MediaTaskCancellationActivation> activate();
  std::optional<MediaTaskCancellationActivation> resolve(
      MediaTaskCancellationChoice choice);
  bool dismiss();
  bool synchronize(const playback_media_processing::Completion& completion);

  const std::optional<MediaTaskCancellationPrompt>& snapshot() const {
    return prompt_;
  }

 private:
  std::optional<MediaTaskCancellationPrompt> prompt_;
};

}  // namespace playback_session
