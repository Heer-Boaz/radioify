#include "playback/session/media_task_cancellation.h"

#include <utility>

#include "core/path_identity.h"
#include "core/runtime_helpers.h"

namespace playback_session {

std::string mediaTaskCancellationTitle(
    playback_media_processing::Operation operation) {
  return std::string("Cancel ") +
         playback_media_processing::operationDisplayName(operation) + "?";
}

std::string mediaTaskCancellationSourceName(
    const MediaTaskCancellationPrompt& prompt) {
  const std::filesystem::path filename = prompt.request.sourceFile.filename();
  return toUtf8String(filename.empty() ? prompt.request.sourceFile : filename);
}

bool MediaTaskCancellationPromptState::open(
    playback_media_processing::CancellationRequest request) {
  if (prompt_ || !request.taskId || request.sourceFile.empty() ||
      !playback_media_processing::cancellationTargetsOperation(
          request.action, request.operation)) {
    return false;
  }
  prompt_ = MediaTaskCancellationPrompt{
      std::move(request), MediaTaskCancellationChoice::KeepRunning};
  return true;
}

bool MediaTaskCancellationPromptState::moveSelection(int direction) {
  if (!prompt_ || direction == 0) return false;
  prompt_->selected =
      prompt_->selected == MediaTaskCancellationChoice::CancelTask
          ? MediaTaskCancellationChoice::KeepRunning
          : MediaTaskCancellationChoice::CancelTask;
  return true;
}

std::optional<MediaTaskCancellationActivation>
MediaTaskCancellationPromptState::activate() {
  if (!prompt_) return std::nullopt;
  return resolve(prompt_->selected);
}

std::optional<MediaTaskCancellationActivation>
MediaTaskCancellationPromptState::resolve(MediaTaskCancellationChoice choice) {
  if (!prompt_) return std::nullopt;
  MediaTaskCancellationActivation activation{
      std::move(prompt_->request),
      choice == MediaTaskCancellationChoice::CancelTask};
  prompt_.reset();
  return activation;
}

bool MediaTaskCancellationPromptState::dismiss() {
  if (!prompt_) return false;
  prompt_.reset();
  return true;
}

bool MediaTaskCancellationPromptState::synchronize(
    const playback_media_processing::Completion& completion) {
  if (!prompt_ || completion.taskId != prompt_->request.taskId ||
      completion.operation != prompt_->request.operation ||
      !samePath(completion.sourceFile, prompt_->request.sourceFile)) {
    return false;
  }
  prompt_.reset();
  return true;
}

}  // namespace playback_session
