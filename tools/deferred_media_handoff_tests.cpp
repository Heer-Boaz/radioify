#include <cstdio>
#include <string>

#include "tui/deferred_media_handoff.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::fprintf(stderr, "deferred_media_handoff_tests: %s\n", message);
  return false;
}

}  // namespace

int main() {
  bool ok = true;
  tui_media_handoff::DeferredCommand<std::string> handoff;

  ok &= expect(handoff.empty(), "new handoff must be empty");
  ok &= expect(handoff.enqueue("second.mp4"),
               "the first external command must be retained");
  ok &= expect(handoff.awaitingRequest(),
               "a retained command must wait safely before request start");
  ok &= expect(!handoff.enqueue("third.mp4"),
               "a second external command must not replace the first");
  ok &= expect(!handoff.markRequestStarted(0),
               "zero must not become a handoff identity");
  ok &= expect(handoff.markRequestStarted(41),
               "the session request id must attach to the retained command");
  ok &= expect(handoff.requestPending(),
               "an identified command must await the matching response");
  ok &= expect(!handoff.cancel(40),
               "a stale cancellation must not discard the command");
  ok &= expect(!handoff.accept(42),
               "a stale acceptance must not consume the command");

  std::optional<std::string> accepted = handoff.accept(41);
  ok &= expect(accepted && *accepted == "second.mp4",
               "the matching acceptance must return the exact command");
  ok &= expect(handoff.empty(),
               "acceptance must atomically clear command and request id");

  ok &= expect(handoff.enqueue("natural-end.mp4"),
               "the state must be reusable after acceptance");
  std::optional<std::string> released = handoff.release();
  ok &= expect(released && *released == "natural-end.mp4",
               "session completion must be able to promote a queued command");
  ok &= expect(handoff.empty(),
               "release must atomically clear all handoff state");

  ok &= expect(handoff.enqueue("cancelled.mp4") &&
                   handoff.markRequestStarted(77) && handoff.cancel(77),
               "the matching cancellation must clear a pending handoff");
  ok &= expect(handoff.empty(),
               "cancellation must leave no stale external command");

  return ok ? 0 : 1;
}
