#include <cstdio>
#include <filesystem>
#include <utility>

#include "playback/session/exit_coordinator.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
  }
  return true;
}

}  // namespace

int main() {
  using namespace playback_session_exit;
  bool ok = true;

  ExitCoordinator local;
  Transition localStop = local.request(StopSession{}, false, true);
  ok &= expect(localStop.handled && localStop.finishSession &&
                   !localStop.quitApplication && !local.pending(),
               "an unconfirmed local stop must finish immediately");

  Transition localQuit = local.request(QuitApplication{}, true, true);
  ok &= expect(localQuit.handled && !localQuit.finishSession &&
                   local.confirmationVisible(),
               "a guarded quit must wait for confirmation");
  Transition confirmedQuit = local.confirm();
  ok &= expect(confirmedQuit.handled && confirmedQuit.finishSession &&
                   confirmedQuit.quitApplication && !local.pending(),
               "confirming a guarded quit must preserve its intent");

  ExitCoordinator transport;
  Transition transportStart = transport.request(
      Transport{PlaybackTransportCommand::Previous}, false, true);
  ok &= expect(transportStart.handled && transportStart.handoffRequest &&
                   transport.awaitingHandoffDecision(),
               "transport must wait for a host decision");
  const RequestId transportId = *transportStart.requestId;
  const auto* transportIntent =
      std::get_if<Transport>(&transportStart.handoffRequest->intent);
  ok &= expect(transportIntent &&
                   transportIntent->command == PlaybackTransportCommand::Previous,
               "transport request must retain its direction");
  Transition rejectedTransport = transport.resolve(transportId, false);
  ok &= expect(rejectedTransport.handled &&
                   !rejectedTransport.finishSession && !transport.pending(),
               "a rejected host request must keep the session alive");

  ExitCoordinator acceptedTransport;
  Transition acceptedStart = acceptedTransport.request(
      Transport{PlaybackTransportCommand::Next}, false, false);
  Transition accepted =
      acceptedTransport.resolve(*acceptedStart.requestId, true);
  ok &= expect(accepted.handled && accepted.finishSession &&
                   !accepted.quitApplication && !acceptedTransport.pending(),
               "an accepted handoff must finish only the current session");

  ExitCoordinator files;
  Transition filesStart = files.request(
      OpenFiles{{std::filesystem::path("first.mp4")}}, true, true);
  ok &= expect(filesStart.handled && !filesStart.handoffRequest &&
                   files.confirmationVisible(),
               "guarded open-files must not reach the host before confirmation");
  Transition filesConfirmed = files.confirm();
  ok &= expect(filesConfirmed.handoffRequest &&
                   files.awaitingHandoffDecision(),
               "confirmed open-files must publish one host request");
  Transition rejectedFiles = files.resolve(*filesConfirmed.requestId, false);
  ok &= expect(rejectedFiles.handled && rejectedFiles.resumePlayback,
               "rejecting a confirmed handoff must resume prior playback");

  ExitCoordinator external;
  Transition externalStart =
      external.request(ExternalHandoff{}, true, true);
  ok &= expect(externalStart.requestId && external.confirmationVisible(),
               "external handoff must allocate a stable request id");
  Transition externalCancel = external.cancel();
  ok &= expect(externalCancel.handled &&
                   externalCancel.handoffCancellation &&
                   externalCancel.handoffCancellation->id ==
                       *externalStart.requestId &&
                   externalCancel.resumePlayback && !external.pending(),
               "cancelling an external handoff must notify its host");

  ExitCoordinator serialization;
  Transition first = serialization.request(ExternalHandoff{}, true, false);
  Transition second = serialization.request(StopSession{}, false, false);
  ok &= expect(first.handled && !second.handled,
               "only one exit transaction may be active at a time");

  return ok ? 0 : 1;
}
