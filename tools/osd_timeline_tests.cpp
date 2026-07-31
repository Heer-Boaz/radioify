#include "playback/session/osd_timeline.h"

#include <chrono>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "osd_timeline_tests: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  using namespace std::chrono_literals;

  bool ok = true;
  playback_session::PlaybackOsdTimeline osd;
  const playback_session::PlaybackOsdTimeline::TimePoint shownAt{100ms};

  ok &= expect(!osd.expire(shownAt) && !osd.nextDeadline(),
               "an empty OSD must not expire or schedule a deadline");

  osd.showControls(shownAt, 1750ms);
  osd.showMessage("Frame copied to clipboard", shownAt, 1500ms);
  const playback_overlay::PlaybackOsdSnapshot published = osd.snapshot();
  ok &= expect(published.controlsVisible,
               "publishing controls must make them visible");
  ok &= expect(published.message &&
                   *published.message == "Frame copied to clipboard",
               "publishing a message must expose its immutable text");
  ok &= expect(osd.nextDeadline() == shownAt + 1500ms,
               "the earliest OSD expiry must drive the next deadline");
  ok &= expect(!osd.expire(shownAt + 1499ms),
               "the OSD must remain unchanged before its deadline");
  ok &= expect(osd.expire(shownAt + 1500ms),
               "expiring the OSD must report a presentation change");

  playback_overlay::PlaybackOsdSnapshot snapshot = osd.snapshot();
  ok &= expect(snapshot.controlsVisible && !snapshot.message,
               "message expiry must preserve longer-lived controls");
  ok &= expect(osd.nextDeadline() == shownAt + 1750ms,
               "the remaining OSD item must own the next deadline");

  osd.showMessage("Frame copy failed", shownAt + 1600ms, 1500ms);
  ok &= expect(published.message &&
                   *published.message == "Frame copied to clipboard",
               "an already-published snapshot must remain immutable");
  ok &= expect(osd.expire(shownAt + 1750ms),
               "control expiry must report a presentation change");
  snapshot = osd.snapshot();
  ok &= expect(!snapshot.controlsVisible && snapshot.message &&
                   *snapshot.message == "Frame copy failed",
               "a replacement message must outlive expired controls");
  ok &= expect(osd.nextDeadline() == shownAt + 3100ms,
               "a replacement message must install its own deadline");

  osd.showControls(shownAt + 1800ms, 1750ms);
  osd.clearControls();
  snapshot = osd.snapshot();
  ok &= expect(!snapshot.controlsVisible && snapshot.message,
               "clearing controls must preserve an independent message");

  osd.clear();
  snapshot = osd.snapshot();
  ok &= expect(!snapshot.controlsVisible && !snapshot.message &&
                   !osd.nextDeadline(),
               "clearing the OSD must remove all state and deadlines");

  return ok ? 0 : 1;
}
