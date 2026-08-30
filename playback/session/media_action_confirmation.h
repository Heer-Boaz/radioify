#pragma once

#include <optional>
#include <string>
#include <variant>

#include "playback/media_action_confirmation_content.h"
#include "playback/media_processing_actions.h"

namespace playback_session {

enum class MediaActionConfirmationChoice {
  Primary,
  Secondary,
};

using MediaActionConfirmationIntent =
    std::variant<playback_media_processing::CancellationRequest,
                 playback_media_processing::AudioSeparationSetupRequest>;

struct MediaActionConfirmationPrompt {
  MediaActionConfirmationIntent intent;
  MediaActionConfirmationChoice selected =
      MediaActionConfirmationChoice::Secondary;
};

struct MediaActionConfirmationActivation {
  MediaActionConfirmationIntent intent;
  bool confirmed = false;
};

std::optional<playback_media_processing::ActionResult>
executeConfirmedMediaAction(
    const playback_media_processing::Actions& actions,
    const MediaActionConfirmationActivation& activation);

playback_media_confirmation::Content mediaActionConfirmationContent(
    const MediaActionConfirmationPrompt& prompt);

// Session-owned modal decision state. Workflow identity stays in the typed
// intent and is emitted only after explicit confirmation; renderers receive a
// presentation-only projection.
class MediaActionConfirmationState {
 public:
  bool open(MediaActionConfirmationIntent intent);
  bool moveSelection(int direction);
  std::optional<MediaActionConfirmationActivation> activate();
  std::optional<MediaActionConfirmationActivation> resolve(
      MediaActionConfirmationChoice choice);
  bool dismiss();
  bool synchronize(const playback_media_processing::Completion& completion);
  bool synchronize(
      const std::optional<playback_media_processing::Activity>& activity);

  const std::optional<MediaActionConfirmationPrompt>& snapshot() const {
    return prompt_;
  }

 private:
  std::optional<MediaActionConfirmationPrompt> prompt_;
};

}  // namespace playback_session
