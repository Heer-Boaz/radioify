#include <iostream>

#include "tui/ui/ui_input_pump.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

bool continuouslyBusySourcesAlternate() {
  FairInputScheduler scheduler;
  InputEvent event{};
  ApplicationInputSurface source = ApplicationInputSurface::Terminal;
  const auto busy = [](ApplicationInputSurface, InputEvent&) { return true; };

  bool ok = true;
  ok &= expect(scheduler.pollNext(busy, event, source) &&
                   source == ApplicationInputSurface::ShellWindow,
               "the shell window has initial priority");
  ok &= expect(scheduler.pollNext(busy, event, source) &&
                   source == ApplicationInputSurface::PlaybackWindow,
               "playback-window input receives the next turn");
  ok &= expect(scheduler.pollNext(busy, event, source) &&
                   source == ApplicationInputSurface::Terminal,
               "terminal input follows both native surfaces");
  ok &= expect(scheduler.pollNext(busy, event, source) &&
                   source == ApplicationInputSurface::ShellWindow,
               "priority keeps rotating while all sources are busy");
  return ok;
}

bool idlePreferredSourceFallsBackWithoutLosingFairness() {
  FairInputScheduler scheduler;
  InputEvent event{};
  ApplicationInputSurface source = ApplicationInputSurface::ShellWindow;
  bool terminalReady = true;
  bool shellReady = false;
  bool playbackReady = false;
  const auto poll = [&](ApplicationInputSurface candidate, InputEvent&) {
    if (candidate == ApplicationInputSurface::Terminal) return terminalReady;
    if (candidate == ApplicationInputSurface::ShellWindow) return shellReady;
    return playbackReady;
  };

  bool ok = true;
  ok &= expect(scheduler.pollNext(poll, event, source) &&
                   source == ApplicationInputSurface::Terminal,
               "an idle preferred source falls back to ready input");
  shellReady = true;
  playbackReady = true;
  ok &= expect(scheduler.pollNext(poll, event, source) &&
                   source == ApplicationInputSurface::ShellWindow,
               "the next ready peer receives the next priority");
  terminalReady = false;
  shellReady = false;
  playbackReady = false;
  ok &= expect(!scheduler.pollNext(poll, event, source),
               "an idle shell reports no synthetic input");
  return ok;
}

bool playbackFloodCannotStarveTerminalInput() {
  FairInputScheduler scheduler;
  InputEvent event{};
  ApplicationInputSurface source = ApplicationInputSurface::ShellWindow;
  int terminalDeliveries = 0;
  int playbackDeliveries = 0;
  const auto poll = [](ApplicationInputSurface candidate, InputEvent&) {
    return candidate != ApplicationInputSurface::ShellWindow;
  };

  for (int turn = 0; turn < 20; ++turn) {
    if (!scheduler.pollNext(poll, event, source)) return false;
    if (source == ApplicationInputSurface::Terminal) ++terminalDeliveries;
    if (source == ApplicationInputSurface::PlaybackWindow) {
      ++playbackDeliveries;
    }
  }
  return expect(terminalDeliveries == 10 && playbackDeliveries == 10,
                "continuous playback input must share turns with terminal "
                "input");
}

}  // namespace

int main() {
  bool ok = true;
  ok &= continuouslyBusySourcesAlternate();
  ok &= idlePreferredSourceFallsBackWithoutLosingFairness();
  ok &= playbackFloodCannotStarveTerminalInput();
  if (!ok) return 1;
  std::cout << "ui_input_pump_tests passed\n";
  return 0;
}
