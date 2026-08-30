#include "playback/session/exit_coordinator.h"

#include <utility>

namespace playback_session_exit {

bool ExitCoordinator::requiresHandoffDecision(const Intent& intent) {
  return std::holds_alternative<Transport>(intent) ||
         std::holds_alternative<OpenFiles>(intent) ||
         std::holds_alternative<ExternalHandoff>(intent);
}

HandoffIntent ExitCoordinator::toHandoffIntent(const Intent& intent) {
  if (const auto* transport = std::get_if<Transport>(&intent)) {
    return *transport;
  }
  if (const auto* openFiles = std::get_if<OpenFiles>(&intent)) {
    return *openFiles;
  }
  return ExternalHandoff{};
}

Transition ExitCoordinator::request(Intent intent, bool confirmationRequired,
                                    bool playbackActive) {
  if (pending_) return {};

  Pending pending;
  pending.intent = std::move(intent);
  pending.resumePlaybackOnCancel = confirmationRequired && playbackActive;
  if (requiresHandoffDecision(pending.intent)) {
    pending.requestId = nextRequestId_++;
  }
  pending.phase = confirmationRequired ? Phase::AwaitingConfirmation
                                       : Phase::AwaitingHandoffDecision;
  pending_.emplace(std::move(pending));

  if (confirmationRequired) {
    Transition transition;
    transition.handled = true;
    transition.requestId = pending_->requestId;
    return transition;
  }
  if (requiresHandoffDecision(pending_->intent)) {
    return requestHandoffDecision();
  }
  return finishLocalExit();
}

Transition ExitCoordinator::confirm() {
  if (!pending_ || pending_->phase != Phase::AwaitingConfirmation) return {};
  if (requiresHandoffDecision(pending_->intent)) {
    pending_->phase = Phase::AwaitingHandoffDecision;
    return requestHandoffDecision();
  }
  return finishLocalExit();
}

Transition ExitCoordinator::cancel() {
  if (!pending_ || pending_->phase != Phase::AwaitingConfirmation) return {};

  Transition transition;
  transition.handled = true;
  transition.resumePlayback = pending_->resumePlaybackOnCancel;
  transition.requestId = pending_->requestId;
  if (pending_->requestId) {
    transition.handoffCancellation =
        HandoffCancellation{*pending_->requestId};
  }
  pending_.reset();
  return transition;
}

Transition ExitCoordinator::resolve(RequestId requestId, bool accepted) {
  if (!pending_ || pending_->phase != Phase::AwaitingHandoffDecision ||
      pending_->requestId != requestId) {
    return {};
  }

  Transition transition;
  transition.handled = true;
  transition.requestId = requestId;
  transition.finishSession = accepted;
  transition.resumePlayback =
      !accepted && pending_->resumePlaybackOnCancel;
  pending_.reset();
  return transition;
}

Transition ExitCoordinator::abortHandoff(RequestId requestId) {
  if (!pending_ || pending_->requestId != requestId) return {};

  Transition transition;
  transition.handled = true;
  transition.requestId = requestId;
  transition.resumePlayback = pending_->resumePlaybackOnCancel;
  pending_.reset();
  return transition;
}

bool ExitCoordinator::confirmationVisible() const {
  return pending_ && pending_->phase == Phase::AwaitingConfirmation;
}

bool ExitCoordinator::awaitingHandoffDecision() const {
  return pending_ && pending_->phase == Phase::AwaitingHandoffDecision;
}

Transition ExitCoordinator::requestHandoffDecision() {
  if (!pending_ || !pending_->requestId ||
      pending_->phase != Phase::AwaitingHandoffDecision ||
      !requiresHandoffDecision(pending_->intent)) {
    return {};
  }

  Transition transition;
  transition.handled = true;
  transition.requestId = pending_->requestId;
  transition.handoffRequest = HandoffRequest{
      *pending_->requestId, toHandoffIntent(pending_->intent)};
  return transition;
}

Transition ExitCoordinator::finishLocalExit() {
  if (!pending_ || requiresHandoffDecision(pending_->intent)) return {};

  Transition transition;
  transition.handled = true;
  transition.finishSession = true;
  transition.quitApplication =
      std::holds_alternative<QuitApplication>(pending_->intent);
  pending_.reset();
  return transition;
}

}  // namespace playback_session_exit
