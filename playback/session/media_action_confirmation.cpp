#include "playback/session/media_action_confirmation.h"

#include <filesystem>
#include <type_traits>
#include <utility>

#include "core/path_identity.h"
#include "core/runtime_helpers.h"

namespace playback_session {
namespace {

std::string sourceName(const std::filesystem::path& sourceFile) {
  const std::filesystem::path filename = sourceFile.filename();
  return toUtf8String(filename.empty() ? sourceFile : filename);
}

}  // namespace

playback_media_confirmation::Content mediaActionConfirmationContent(
    const MediaActionConfirmationPrompt& prompt) {
  if (const auto* cancellation =
          std::get_if<playback_media_processing::CancellationRequest>(
              &prompt.intent)) {
    return playback_media_confirmation::cancellationContent(
        cancellation->operation, sourceName(cancellation->sourceFile));
  }
  return playback_media_confirmation::audioSeparationSetupContent();
}

std::optional<playback_media_processing::ActionResult>
executeConfirmedMediaAction(
    const playback_media_processing::Actions& actions,
    const MediaActionConfirmationActivation& activation) {
  if (!activation.confirmed) return std::nullopt;
  return std::visit(
      [&](const auto& intent) {
        using Intent = std::decay_t<decltype(intent)>;
        if constexpr (std::is_same_v<
                          Intent,
                          playback_media_processing::CancellationRequest>) {
          return actions.confirmCancellation(intent);
        } else {
          return actions.confirmAudioSeparationSetup(intent);
        }
      },
      activation.intent);
}

bool MediaActionConfirmationState::open(
    MediaActionConfirmationIntent intent) {
  if (prompt_) return false;
  if (const auto* cancellation =
          std::get_if<playback_media_processing::CancellationRequest>(
              &intent)) {
    if (!cancellation->taskId || cancellation->sourceFile.empty()) {
      return false;
    }
  } else if (std::get<
                 playback_media_processing::AudioSeparationSetupRequest>(
                 intent)
                 .sourceFile.empty()) {
    return false;
  }
  prompt_ = MediaActionConfirmationPrompt{
      std::move(intent), MediaActionConfirmationChoice::Secondary};
  return true;
}

bool MediaActionConfirmationState::moveSelection(int direction) {
  if (!prompt_ || direction == 0) return false;
  prompt_->selected =
      prompt_->selected == MediaActionConfirmationChoice::Primary
          ? MediaActionConfirmationChoice::Secondary
          : MediaActionConfirmationChoice::Primary;
  return true;
}

std::optional<MediaActionConfirmationActivation>
MediaActionConfirmationState::activate() {
  if (!prompt_) return std::nullopt;
  return resolve(prompt_->selected);
}

std::optional<MediaActionConfirmationActivation>
MediaActionConfirmationState::resolve(MediaActionConfirmationChoice choice) {
  if (!prompt_) return std::nullopt;
  MediaActionConfirmationActivation activation{
      std::move(prompt_->intent),
      choice == MediaActionConfirmationChoice::Primary};
  prompt_.reset();
  return activation;
}

bool MediaActionConfirmationState::dismiss() {
  if (!prompt_) return false;
  prompt_.reset();
  return true;
}

bool MediaActionConfirmationState::synchronize(
    const playback_media_processing::Completion& completion) {
  if (!prompt_) return false;
  const auto* cancellation =
      std::get_if<playback_media_processing::CancellationRequest>(
          &prompt_->intent);
  if (!cancellation || completion.taskId != cancellation->taskId ||
      completion.operation != cancellation->operation ||
      !samePath(completion.sourceFile, cancellation->sourceFile)) {
    return false;
  }
  prompt_.reset();
  return true;
}

bool MediaActionConfirmationState::synchronize(
    const std::optional<playback_media_processing::Activity>& activity) {
  if (!prompt_) return false;

  bool stillCurrent = false;
  if (const auto* cancellation =
          std::get_if<playback_media_processing::CancellationRequest>(
              &prompt_->intent)) {
    stillCurrent =
        activity && activity->taskId == cancellation->taskId &&
        activity->operation == cancellation->operation &&
        samePath(activity->sourceFile, cancellation->sourceFile) &&
        activity->cancellable && !activity->cancelling;
  } else {
    // Provider setup can only start while the application task slot is idle.
    // If another surface fills that slot, the pending decision is obsolete.
    stillCurrent = !activity;
  }
  if (stillCurrent) return false;
  prompt_.reset();
  return true;
}

}  // namespace playback_session
