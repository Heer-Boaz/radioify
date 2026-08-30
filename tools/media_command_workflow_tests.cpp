#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "tui/media_command_workflow.h"

namespace {

using tui_media_command::DeferralReason;
using tui_media_command::HandoffProgress;
using tui_media_command::HandoffRequestResolution;
using tui_media_command::Workflow;

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "media_command_workflow_tests: " << message << '\n';
  return false;
}

class RecordingHandoffEndpoint final
    : public playback_session::HandoffEndpoint {
 public:
  bool handoffRequestDeferred() const override { return deferred; }

  std::optional<playback_session_exit::RequestId> requestHandoff() override {
    ++requestCount;
    if (rejectRequest || pendingRequest) return std::nullopt;
    const playback_session_exit::RequestId requestId =
        returnInvalidIdentity ? 0 : nextRequestId++;
    pendingRequest = requestId;
    return requestId;
  }

  bool resolveHandoff(playback_session_exit::RequestId requestId,
                      bool accepted) override {
    resolutions.emplace_back(requestId, accepted);
    if (!pendingRequest || *pendingRequest != requestId ||
        !resolveSucceeds) {
      return false;
    }
    pendingRequest.reset();
    return true;
  }

  bool abortHandoff(
      playback_session_exit::RequestId requestId) override {
    aborts.push_back(requestId);
    if (!pendingRequest || *pendingRequest != requestId ||
        !abortSucceeds) {
      return false;
    }
    pendingRequest.reset();
    return true;
  }

  bool cancelFromSession(
      playback_session_exit::RequestId requestId) {
    if (!pendingRequest || *pendingRequest != requestId) return false;
    pendingRequest.reset();
    return true;
  }

  bool deferred = false;
  bool rejectRequest = false;
  bool returnInvalidIdentity = false;
  bool resolveSucceeds = true;
  bool abortSucceeds = true;
  playback_session_exit::RequestId nextRequestId = 41;
  std::optional<playback_session_exit::RequestId> pendingRequest;
  int requestCount = 0;
  std::vector<std::pair<playback_session_exit::RequestId, bool>> resolutions;
  std::vector<playback_session_exit::RequestId> aborts;
};

bool isQuit(const tui_media_command::Command& command) {
  return std::holds_alternative<tui_media_command::QuitApplication>(command);
}

const tui_media_activation::OpenDirectory* openDirectory(
    const std::optional<tui_media_command::Command>& command) {
  return command
             ? std::get_if<tui_media_activation::OpenDirectory>(&*command)
             : nullptr;
}

bool dispatchOwnershipIsExclusiveAndTransactional() {
  bool ok = true;
  Workflow workflow;
  ok &= expect(workflow.idle() && !workflow.dispatching() &&
                   !workflow.hasDeferredCommand() &&
                   !workflow.handoffActive(),
               "a new owner-thread command workflow must be idle");

  std::optional<Workflow::DispatchLease> dispatch =
      workflow.beginDispatch();
  ok &= expect(dispatch && workflow.dispatching() &&
                   !workflow.beginDispatch(),
               "dispatch ownership must be exclusive");
  ok &= expect(
      workflow.queueDuringDispatch(
          tui_media_command::QuitApplication{},
          DeferralReason::AfterCurrentDispatch) &&
          !workflow.queueDuringDispatch(
              tui_media_command::QuitApplication{},
              DeferralReason::AfterCurrentDispatch),
      "one active dispatch may retain one explicit follow-up command");
  std::optional<tui_media_command::Command> followup =
      workflow.takeDuringDispatch();
  ok &= expect(followup && isQuit(*followup),
               "the dispatcher must consume the exact retained follow-up");
  dispatch.reset();
  ok &= expect(workflow.idle(),
               "finishing a dispatch without retained work must return idle");

  dispatch = workflow.beginDispatch();
  ok &= expect(
      dispatch && workflow.queueDuringDispatch(
                      tui_media_activation::OpenDirectory{"superseded"},
                      DeferralReason::AfterCurrentDispatch),
      "an active dispatch must retain its original follow-up before a "
      "completed session-open reports quit");
  workflow.preemptWithQuitAfterSessionExit();
  followup = workflow.takeDuringDispatch();
  ok &= expect(followup && isQuit(*followup),
               "a committed quit must replace the older follow-up");
  dispatch.reset();
  return ok && workflow.idle();
}

bool dispatchDeferralPublishesOwnedWork() {
  bool ok = true;
  Workflow workflow;
  std::optional<Workflow::DispatchLease> dispatch =
      workflow.beginDispatch();
  ok &= expect(
      dispatch && workflow.queueDuringDispatch(
                      tui_media_command::QuitApplication{},
                      DeferralReason::InteractivePlayback),
      "resource suspension must retain the current command in the dispatch "
      "transaction");
  dispatch.reset();
  ok &= expect(workflow.hasDeferredCommand() &&
                   workflow.deferredReason() ==
                       DeferralReason::InteractivePlayback,
               "dispatch completion must publish retained work with its "
               "resource-deferral reason");
  std::optional<tui_media_command::Command> resumed =
      workflow.takeDeferred();
  return ok && expect(resumed && isQuit(*resumed) && workflow.idle(),
                      "deferred work must transfer exactly once");
}

bool retainedWorkCanBeDiscardedAtTheEndpointBoundary() {
  bool ok = true;
  Workflow workflow;
  std::optional<Workflow::DispatchLease> dispatch =
      workflow.beginDispatch();
  ok &= expect(
      dispatch && workflow.queueDuringDispatch(
                      tui_media_activation::OpenDirectory{"queued"},
                      DeferralReason::AfterCurrentDispatch),
      "the reset workflow requires queued work during an active dispatch");
  workflow.discardRetainedCommands();
  ok &= expect(workflow.dispatching() && !workflow.takeDuringDispatch(),
               "discarding retained work must not pretend to interrupt the "
               "currently executing command");
  dispatch.reset();
  ok &= expect(workflow.idle(),
               "the active dispatch must finish without hidden follow-up");

  {
    RecordingHandoffEndpoint endpoint;
    ok &= expect(
        workflow.beginHandoff(
            tui_media_activation::OpenDirectory{"endpoint-owned"},
            endpoint) == HandoffProgress::RequestStarted,
        "the lifecycle reset requires one endpoint-owned handoff");
  }
  workflow.discardRetainedCommands();
  return ok && expect(workflow.idle(),
                      "closing the endpoint must allow all retained command "
                      "ownership to be discarded");
}

bool externalHandoffRequiresEndpointAcknowledgement() {
  bool ok = true;
  Workflow workflow;
  RecordingHandoffEndpoint endpoint;
  endpoint.deferred = true;

  const HandoffProgress waiting = workflow.beginHandoff(
      tui_media_activation::OpenDirectory{"handoff-target"}, endpoint);
  ok &= expect(waiting == HandoffProgress::WaitingForInteraction &&
                   workflow.handoffAwaitingRequest() &&
                   endpoint.requestCount == 0,
               "an interaction gate must retain the command without "
               "inventing a request identity");
  endpoint.deferred = false;
  const HandoffProgress requested = workflow.resumeHandoff(endpoint);
  constexpr playback_session_exit::RequestId requestId = 41;
  ok &= expect(requested == HandoffProgress::RequestStarted &&
                   workflow.handoffActive() &&
                   !workflow.handoffAwaitingRequest() &&
                   endpoint.requestCount == 1,
               "the live endpoint must publish the handoff identity");

  ok &= expect(
      workflow.resolveExternalHandoffRequest(requestId, endpoint) ==
              HandoffRequestResolution::Accepted &&
          endpoint.resolutions ==
              std::vector<
                  std::pair<playback_session_exit::RequestId, bool>>{
                  {requestId, true}} &&
          !endpoint.pendingRequest,
      "the command may commit only after the correlated request receives "
      "one endpoint acknowledgement");
  ok &= expect(workflow.deferredReason() ==
                   DeferralReason::VideoSessionExit,
               "endpoint acceptance must retain work until session exit");
  const std::optional<tui_media_command::Command> resumed =
      workflow.takeDeferred();
  const auto* directory = openDirectory(resumed);
  return ok && expect(directory && directory->path == "handoff-target" &&
                          workflow.idle(),
                      "session exit must release the exact accepted command");
}

bool sessionOriginatedHandoffCommitsAfterAcknowledgement() {
  bool ok = true;
  Workflow workflow;
  RecordingHandoffEndpoint endpoint;
  const std::optional<playback_session_exit::RequestId> pendingRequest =
      endpoint.requestHandoff();
  const playback_session_exit::RequestId requestId =
      pendingRequest.value_or(0);

  ok &= expect(
      pendingRequest && workflow.resolveSessionHandoffRequest(
          tui_media_activation::OpenDirectory{"session-command"}, requestId,
          endpoint) == HandoffRequestResolution::Accepted &&
          endpoint.resolutions ==
              std::vector<
                  std::pair<playback_session_exit::RequestId, bool>>{
                  {requestId, true}} &&
          workflow.hasDeferredCommand(),
      "a session-originated command must become runnable only after endpoint "
      "acceptance");
  const std::optional<tui_media_command::Command> resumed =
      workflow.takeDeferred();
  const auto* directory = openDirectory(resumed);
  return ok && expect(directory && directory->path == "session-command" &&
                          workflow.idle(),
                      "the accepted session command must preserve its payload");
}

bool cancellationAndFailedAcknowledgementReleaseOwnership() {
  bool ok = true;
  Workflow workflow;
  RecordingHandoffEndpoint endpoint;
  constexpr playback_session_exit::RequestId cancelledRequest = 41;

  ok &= expect(
      workflow.beginHandoff(
          tui_media_activation::OpenDirectory{"cancelled-target"}, endpoint) ==
              HandoffProgress::RequestStarted &&
          !workflow.cancelHandoff(cancelledRequest + 1) &&
          endpoint.cancelFromSession(cancelledRequest) &&
          workflow.cancelHandoff(cancelledRequest) && workflow.idle(),
      "handoff cancellation must be identity-bound and one-shot");

  endpoint.resolveSucceeds = false;
  constexpr playback_session_exit::RequestId rejectedRequest = 42;
  ok &= expect(
      workflow.beginHandoff(
          tui_media_activation::OpenDirectory{"endpoint-rejected"},
          endpoint) == HandoffProgress::RequestStarted &&
          workflow.resolveExternalHandoffRequest(rejectedRequest, endpoint) ==
              HandoffRequestResolution::
                  AbortedAfterAcknowledgementFailure &&
          workflow.idle() &&
          endpoint.aborts ==
              std::vector<playback_session_exit::RequestId>{rejectedRequest} &&
          !endpoint.pendingRequest,
      "a failed endpoint acknowledgement must explicitly abort the retained "
      "session request before releasing workflow ownership");

  endpoint.resolveSucceeds = true;
  return ok && expect(
                   workflow.beginHandoff(
                       tui_media_activation::OpenDirectory{"recovered"},
                       endpoint) == HandoffProgress::RequestStarted &&
                       workflow.cancelHandoff(43) && workflow.idle(),
                   "an acknowledgement failure must not poison later work");
}

bool unrecoverableAcknowledgementBlocksUntilSessionExit() {
  bool ok = true;
  Workflow workflow;
  {
    RecordingHandoffEndpoint endpoint;
    endpoint.resolveSucceeds = false;
    endpoint.abortSucceeds = false;
    constexpr playback_session_exit::RequestId requestId = 41;

    ok &= expect(
        workflow.beginHandoff(
            tui_media_activation::OpenDirectory{"faulted"}, endpoint) ==
                HandoffProgress::RequestStarted &&
            workflow.resolveExternalHandoffRequest(requestId, endpoint) ==
                HandoffRequestResolution::ProtocolFault &&
            workflow.handoffFaulted() && !workflow.idle(),
        "an acknowledgement that cannot be aborted must fence out later "
        "work");
    const int requestsBeforeBlockedCommand = endpoint.requestCount;
    ok &= expect(
        workflow.beginHandoff(
            tui_media_activation::OpenDirectory{"must-remain-blocked"},
            endpoint) == HandoffProgress::Busy &&
            endpoint.requestCount == requestsBeforeBlockedCommand,
        "a faulted handoff must not issue another endpoint request");
  }
  ok &= expect(!workflow.releaseHandoffForSessionExit() && workflow.idle(),
               "actual session destruction must clear the fault without "
               "running the abandoned command");
  return ok;
}

bool sessionExitDefinesQuitPreemptionBoundary() {
  bool ok = true;
  Workflow workflow;
  {
    RecordingHandoffEndpoint endpoint;
    endpoint.deferred = true;
    ok &= expect(
        workflow.beginHandoff(
            tui_media_activation::OpenDirectory{"completed-without-request"},
            endpoint) == HandoffProgress::WaitingForInteraction,
        "the session-exit boundary requires retained work before request "
        "publication");
  }
  ok &= expect(workflow.releaseHandoffForSessionExit() &&
                   workflow.deferredReason() ==
                       DeferralReason::VideoSessionExit,
               "session destruction must publish its retained command");
  workflow.preemptWithQuitAfterSessionExit();
  std::optional<tui_media_command::Command> resumed =
      workflow.takeDeferred();
  ok &= expect(resumed && isQuit(*resumed) && workflow.idle(),
               "committed quit must replace older work after session exit");

  Workflow requestedWorkflow;
  {
    RecordingHandoffEndpoint requestedEndpoint;
    ok &= expect(
        requestedWorkflow.beginHandoff(
            tui_media_activation::OpenDirectory{"ended-negotiation"},
            requestedEndpoint) == HandoffProgress::RequestStarted &&
            requestedWorkflow.handoffActive(),
        "session-exit preemption must also cover a published request "
        "identity");
  }
  requestedWorkflow.preemptWithQuitAfterSessionExit();
  resumed = requestedWorkflow.takeDeferred();
  return ok && expect(resumed && isQuit(*resumed) &&
                          requestedWorkflow.idle(),
                      "destroying the endpoint must make quit the sole "
                      "remaining command");
}

bool rejectedRequestsLeaveNoHiddenWork() {
  bool ok = true;
  Workflow workflow;
  RecordingHandoffEndpoint endpoint;
  endpoint.rejectRequest = true;
  ok &= expect(
      workflow.beginHandoff(
          tui_media_activation::OpenDirectory{"request-rejected"},
          endpoint) == HandoffProgress::RequestRejected &&
          workflow.idle(),
      "an endpoint request rejection must return the workflow to idle");

  endpoint.rejectRequest = false;
  endpoint.returnInvalidIdentity = true;
  ok &= expect(
      workflow.beginHandoff(
          tui_media_activation::OpenDirectory{"invalid-identity"},
          endpoint) == HandoffProgress::RequestRejected &&
          workflow.idle() &&
          endpoint.resolutions ==
              std::vector<
                  std::pair<playback_session_exit::RequestId, bool>>{
                  {0, false}},
      "an invalid endpoint identity must be rejected and acknowledged once");

  endpoint.returnInvalidIdentity = false;
  const int requestsBeforeIdleResume = endpoint.requestCount;
  ok &= expect(workflow.resumeHandoff(endpoint) ==
                       HandoffProgress::NotAwaitingRequest &&
                   endpoint.requestCount == requestsBeforeIdleResume,
               "an idle workflow must not invent a handoff request");

  std::optional<Workflow::DispatchLease> dispatch =
      workflow.beginDispatch();
  return ok && expect(
                   dispatch &&
                       workflow.beginHandoff(
                           tui_media_activation::OpenDirectory{"busy"},
                           endpoint) == HandoffProgress::Busy &&
                       endpoint.requestCount == requestsBeforeIdleResume,
                   "an active dispatch must reject a competing handoff "
                   "without endpoint side effects");
}

}  // namespace

int main() {
  bool ok = true;
  ok &= dispatchOwnershipIsExclusiveAndTransactional();
  ok &= dispatchDeferralPublishesOwnedWork();
  ok &= retainedWorkCanBeDiscardedAtTheEndpointBoundary();
  ok &= externalHandoffRequiresEndpointAcknowledgement();
  ok &= sessionOriginatedHandoffCommitsAfterAcknowledgement();
  ok &= cancellationAndFailedAcknowledgementReleaseOwnership();
  ok &= unrecoverableAcknowledgementBlocksUntilSessionExit();
  ok &= sessionExitDefinesQuitPreemptionBoundary();
  ok &= rejectedRequestsLeaveNoHiddenWork();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
