#include <cstdio>
#include <optional>
#include <utility>
#include <vector>

#include "app/video_handoff_controller.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::fprintf(stderr, "video_handoff_controller_tests: %s\n", message);
  return false;
}

struct TestCommand {
  int value = 0;
};

class RecordingHandoffEndpoint final
    : public playback_session::HandoffEndpoint {
 public:
  bool handoffRequestDeferred() const override { return deferred; }

  std::optional<playback_session_exit::RequestId> requestHandoff() override {
    ++requestCount;
    return nextRequestId;
  }

  bool resolveHandoff(playback_session_exit::RequestId requestId,
                      bool accepted) override {
    resolutions.emplace_back(requestId, accepted);
    return true;
  }

  bool deferred = true;
  std::optional<playback_session_exit::RequestId> nextRequestId = 41;
  int requestCount = 0;
  std::vector<std::pair<playback_session_exit::RequestId, bool>> resolutions;
};

}  // namespace

int main() {
  bool ok = true;
  application_playback::VideoHandoffController<TestCommand> handoff;
  RecordingHandoffEndpoint endpoint;

  ok &= expect(handoff.empty(), "new handoff must be empty");
  ok &= expect(handoff.enqueue(TestCommand{2}),
               "the first external command must be retained");
  ok &= expect(handoff.awaitingRequest(),
               "a retained command must wait safely before request start");
  ok &= expect(!handoff.enqueue(TestCommand{3}),
               "a second external command must not replace the first");
  ok &= expect(
      handoff.tryStart(endpoint) ==
              application_playback::VideoHandoffStart::
                  WaitingForInteraction &&
          endpoint.requestCount == 0 && handoff.awaitingRequest(),
      "a session-owned interaction must defer the request without side "
      "effects");

  endpoint.deferred = false;
  ok &= expect(handoff.tryStart(endpoint) ==
                   application_playback::VideoHandoffStart::RequestStarted &&
                   endpoint.requestCount == 1,
               "the live endpoint must provide the handoff identity");
  ok &= expect(handoff.requestPending(),
               "an identified command must await the matching response");
  ok &= expect(!handoff.cancel(40),
               "a stale cancellation must not discard the command");
  ok &= expect(!handoff.accept(42),
               "a stale acceptance must not consume the command");

  std::optional<TestCommand> accepted = handoff.accept(41);
  ok &= expect(accepted && accepted->value == 2,
               "the matching acceptance must return the exact command");
  ok &= expect(handoff.empty(),
               "acceptance must atomically clear command and request id");

  ok &= expect(handoff.enqueue(TestCommand{4}),
               "the state must be reusable after acceptance");
  std::optional<TestCommand> released = handoff.release();
  ok &= expect(released && released->value == 4,
               "session completion must be able to promote a queued command");
  ok &= expect(handoff.empty(),
               "release must atomically clear all handoff state");

  endpoint.nextRequestId = 77;
  ok &= expect(handoff.enqueue(TestCommand{5}) &&
                   handoff.tryStart(endpoint) ==
                       application_playback::VideoHandoffStart::
                           RequestStarted &&
                   handoff.cancel(77),
               "the matching cancellation must clear a pending handoff");
  ok &= expect(handoff.empty(),
               "cancellation must leave no stale external command");

  endpoint.nextRequestId.reset();
  ok &= expect(handoff.enqueue(TestCommand{6}) &&
                   handoff.tryStart(endpoint) ==
                       application_playback::VideoHandoffStart::Failed &&
                   handoff.empty(),
               "a rejected request must atomically abandon its command");

  endpoint.nextRequestId = 0;
  ok &= expect(
      handoff.enqueue(TestCommand{7}) &&
          handoff.tryStart(endpoint) ==
              application_playback::VideoHandoffStart::Failed &&
          handoff.empty() && !endpoint.resolutions.empty() &&
          endpoint.resolutions.back() ==
              std::pair<playback_session_exit::RequestId, bool>{0, false},
      "an invalid endpoint identity must be rejected and resolved exactly "
      "once");

  const int requestCountBeforeEmptyStart = endpoint.requestCount;
  ok &= expect(handoff.tryStart(endpoint) ==
                       application_playback::VideoHandoffStart::
                           NoPendingCommand &&
                   endpoint.requestCount == requestCountBeforeEmptyStart,
               "an empty controller must not invoke the endpoint");

  return ok ? 0 : 1;
}
