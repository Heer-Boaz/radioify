#include "tui/media_command_workflow.h"

#include <type_traits>
#include <utility>

namespace tui_media_command {

namespace {

struct Idle {};

struct DeferredCommand {
  Command command;
  DeferralReason reason = DeferralReason::AfterCurrentDispatch;
};

struct Dispatching {
  std::optional<DeferredCommand> queued;
};

struct Deferred {
  DeferredCommand pending;
};

struct NegotiatingHandoff {
  Command command;
  std::optional<playback_session_exit::RequestId> requestId;
};

struct HandoffFault {
  playback_session_exit::RequestId requestId = 0;
};

using State =
    std::variant<Idle, Dispatching, Deferred, NegotiatingHandoff,
                 HandoffFault>;

static_assert(std::is_nothrow_move_constructible_v<Command>);

}  // namespace

struct Workflow::Impl {
  State state;
};

Workflow::Workflow() : impl_(std::make_unique<Impl>()) {}

Workflow::~Workflow() = default;

Workflow::DispatchLease::~DispatchLease() { finish(); }

Workflow::DispatchLease::DispatchLease(DispatchLease&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)) {}

Workflow::DispatchLease& Workflow::DispatchLease::operator=(
    DispatchLease&& other) noexcept {
  if (this == &other) return *this;
  finish();
  owner_ = std::exchange(other.owner_, nullptr);
  return *this;
}

void Workflow::DispatchLease::finish() noexcept {
  Workflow* owner = std::exchange(owner_, nullptr);
  if (owner) owner->finishDispatch();
}

bool Workflow::idle() const noexcept {
  return std::holds_alternative<Idle>(impl_->state);
}

bool Workflow::dispatching() const noexcept {
  return std::holds_alternative<Dispatching>(impl_->state);
}

bool Workflow::hasDeferredCommand() const noexcept {
  return std::holds_alternative<Deferred>(impl_->state);
}

std::optional<DeferralReason> Workflow::deferredReason() const noexcept {
  const auto* deferred = std::get_if<Deferred>(&impl_->state);
  return deferred ? std::optional<DeferralReason>(deferred->pending.reason)
                  : std::nullopt;
}

bool Workflow::handoffActive() const noexcept {
  return std::holds_alternative<NegotiatingHandoff>(impl_->state);
}

bool Workflow::handoffAwaitingRequest() const noexcept {
  const auto* handoff = std::get_if<NegotiatingHandoff>(&impl_->state);
  return handoff && !handoff->requestId;
}

bool Workflow::handoffFaulted() const noexcept {
  return std::holds_alternative<HandoffFault>(impl_->state);
}

std::optional<Workflow::DispatchLease> Workflow::beginDispatch() {
  if (!idle()) return std::nullopt;
  impl_->state.emplace<Dispatching>();
  return DispatchLease(*this);
}

bool Workflow::queueDuringDispatch(Command command,
                                   DeferralReason reason) {
  auto* active = std::get_if<Dispatching>(&impl_->state);
  if (!active || active->queued) return false;
  active->queued.emplace(
      DeferredCommand{std::move(command), reason});
  return true;
}

std::optional<Command> Workflow::takeDuringDispatch() {
  auto* active = std::get_if<Dispatching>(&impl_->state);
  if (!active || !active->queued) return std::nullopt;
  Command command = std::move(active->queued->command);
  active->queued.reset();
  return command;
}

void Workflow::discardQueuedDuringDispatch() noexcept {
  if (auto* active = std::get_if<Dispatching>(&impl_->state)) {
    active->queued.reset();
  }
}

std::optional<Command> Workflow::takeDeferred() {
  auto* deferred = std::get_if<Deferred>(&impl_->state);
  if (!deferred) return std::nullopt;
  Command command = std::move(deferred->pending.command);
  impl_->state.emplace<Idle>();
  return command;
}

HandoffProgress Workflow::beginHandoff(
    Command command, playback_session::HandoffEndpoint& endpoint) {
  if (!idle()) return HandoffProgress::Busy;
  impl_->state.emplace<NegotiatingHandoff>(
      NegotiatingHandoff{std::move(command), std::nullopt});
  return resumeHandoff(endpoint);
}

HandoffProgress Workflow::resumeHandoff(
    playback_session::HandoffEndpoint& endpoint) {
  auto* handoff = std::get_if<NegotiatingHandoff>(&impl_->state);
  if (!handoff || handoff->requestId) {
    return HandoffProgress::NotAwaitingRequest;
  }
  if (endpoint.handoffRequestDeferred()) {
    return HandoffProgress::WaitingForInteraction;
  }

  const std::optional<playback_session_exit::RequestId> requestId =
      endpoint.requestHandoff();
  if (!requestId || *requestId == 0) {
    if (requestId) {
      const HandoffRequestResolution resolution =
          declineHandoffRequest(*requestId, endpoint);
      if (resolution == HandoffRequestResolution::ProtocolFault) {
        return HandoffProgress::ProtocolFault;
      }
    }
    impl_->state.emplace<Idle>();
    return HandoffProgress::RequestRejected;
  }
  handoff = std::get_if<NegotiatingHandoff>(&impl_->state);
  handoff->requestId = *requestId;
  return HandoffProgress::RequestStarted;
}

bool Workflow::matchesHandoff(
    playback_session_exit::RequestId requestId) const noexcept {
  const auto* handoff = std::get_if<NegotiatingHandoff>(&impl_->state);
  return handoff && handoff->requestId == requestId;
}

HandoffRequestResolution Workflow::resolveExternalHandoffRequest(
    playback_session_exit::RequestId requestId,
    playback_session::HandoffEndpoint& endpoint) {
  auto* handoff = std::get_if<NegotiatingHandoff>(&impl_->state);
  if (!handoff || handoff->requestId != requestId) {
    return declineHandoffRequest(requestId, endpoint);
  }
  if (!endpoint.resolveHandoff(requestId, true)) {
    return recoverRejectedAcknowledgement(requestId, endpoint, true);
  }
  Command command = std::move(handoff->command);
  impl_->state.emplace<Deferred>(Deferred{DeferredCommand{
      std::move(command), DeferralReason::VideoSessionExit}});
  return HandoffRequestResolution::Accepted;
}

HandoffRequestResolution Workflow::resolveSessionHandoffRequest(
    Command command, playback_session_exit::RequestId requestId,
    playback_session::HandoffEndpoint& endpoint) {
  if (!idle()) return declineHandoffRequest(requestId, endpoint);
  if (!endpoint.resolveHandoff(requestId, true)) {
    return recoverRejectedAcknowledgement(requestId, endpoint, false);
  }
  impl_->state.emplace<Deferred>(Deferred{DeferredCommand{
      std::move(command), DeferralReason::VideoSessionExit}});
  return HandoffRequestResolution::Accepted;
}

HandoffRequestResolution Workflow::declineHandoffRequest(
    playback_session_exit::RequestId requestId,
    playback_session::HandoffEndpoint& endpoint) {
  const bool matchingExternalRequest = matchesHandoff(requestId);
  const bool resolved = endpoint.resolveHandoff(requestId, false);
  if (!resolved) {
    return recoverRejectedAcknowledgement(
        requestId, endpoint, matchingExternalRequest);
  }
  if (matchingExternalRequest) impl_->state.emplace<Idle>();
  return HandoffRequestResolution::Declined;
}

HandoffRequestResolution Workflow::recoverRejectedAcknowledgement(
    playback_session_exit::RequestId requestId,
    playback_session::HandoffEndpoint& endpoint,
    bool discardMatchingCommand) {
  if (endpoint.abortHandoff(requestId)) {
    if (discardMatchingCommand) impl_->state.emplace<Idle>();
    return HandoffRequestResolution::
        AbortedAfterAcknowledgementFailure;
  }
  impl_->state.emplace<HandoffFault>(HandoffFault{requestId});
  return HandoffRequestResolution::ProtocolFault;
}

bool Workflow::cancelHandoff(
    playback_session_exit::RequestId requestId) noexcept {
  const auto* fault = std::get_if<HandoffFault>(&impl_->state);
  if (!matchesHandoff(requestId) &&
      (!fault || fault->requestId != requestId)) {
    return false;
  }
  impl_->state.emplace<Idle>();
  return true;
}

bool Workflow::releaseHandoffForSessionExit() {
  if (std::holds_alternative<HandoffFault>(impl_->state)) {
    impl_->state.emplace<Idle>();
    return false;
  }
  auto* handoff = std::get_if<NegotiatingHandoff>(&impl_->state);
  if (!handoff) return false;
  Command command = std::move(handoff->command);
  impl_->state.emplace<Deferred>(Deferred{DeferredCommand{
      std::move(command), DeferralReason::VideoSessionExit}});
  return true;
}

void Workflow::preemptWithQuitAfterSessionExit() {
  Command quit = QuitApplication{};
  if (auto* active = std::get_if<Dispatching>(&impl_->state)) {
    active->queued.emplace(DeferredCommand{
        std::move(quit), DeferralReason::AfterCurrentDispatch});
    return;
  }
  impl_->state.emplace<Deferred>(Deferred{DeferredCommand{
      std::move(quit), DeferralReason::VideoSessionExit}});
}

void Workflow::discardRetainedCommands() noexcept {
  if (auto* active = std::get_if<Dispatching>(&impl_->state)) {
    active->queued.reset();
    return;
  }
  impl_->state.emplace<Idle>();
}

void Workflow::finishDispatch() noexcept {
  auto* active = std::get_if<Dispatching>(&impl_->state);
  if (!active) return;
  if (!active->queued) {
    impl_->state.emplace<Idle>();
    return;
  }
  DeferredCommand pending = std::move(*active->queued);
  impl_->state.emplace<Deferred>(Deferred{std::move(pending)});
}

}  // namespace tui_media_command
