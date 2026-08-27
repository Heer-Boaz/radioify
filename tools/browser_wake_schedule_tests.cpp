#include "tui/ui/browser_wake_schedule.h"

#include <chrono>
#include <cstdio>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::fprintf(stderr, "browser_wake_schedule_tests: %s\n", message);
  return false;
}

}  // namespace

int main() {
  using namespace std::chrono_literals;

  bool ok = true;
  const wake_schedule::TimePoint now{1250ms};
  const wake_schedule::TimePoint lastPresented{1200ms};

  ok &= expect(browser_wake_schedule::searchCaretOn(
                   wake_schedule::TimePoint{499ms}) &&
                   !browser_wake_schedule::searchCaretOn(
                       wake_schedule::TimePoint{500ms}),
               "caret visibility and wake phases must share one policy");

  ok &= expect(!browser_wake_schedule::nextDeadline(
                   now, lastPresented, browser_wake_schedule::Activity{}),
               "an inactive browser must not request periodic wakeups");

  browser_wake_schedule::Activity search;
  search.searchCaretVisible = true;
  ok &= expect(browser_wake_schedule::nextDeadline(now, lastPresented,
                                                    search) ==
                   wake_schedule::TimePoint{1500ms},
               "the search caret must wake on its exact phase boundary");

  browser_wake_schedule::Activity transport;
  transport.transportProgressVisible = true;
  ok &= expect(browser_wake_schedule::nextDeadline(now, lastPresented,
                                                    transport) ==
                   wake_schedule::TimePoint{1300ms},
               "visible transport progress must own a named 100 ms cadence");

  browser_wake_schedule::Activity combined = transport;
  combined.melodyMonitorVisible = true;
  combined.audioPictureInPictureVisible = true;
  ok &= expect(browser_wake_schedule::nextDeadline(now, lastPresented,
                                                    combined) ==
                   wake_schedule::TimePoint{1250ms},
               "the earliest active presentation cadence must win");

  return ok ? 0 : 1;
}
