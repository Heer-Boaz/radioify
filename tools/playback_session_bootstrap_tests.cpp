#include <chrono>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <variant>

#include "playback/session/bootstrap.h"
#include "tui/ui/video_transition_view.h"

namespace {

wake_schedule::TimePoint fakeNowValue;

wake_schedule::TimePoint fakeNow() { return fakeNowValue; }

class FakeOpeningBackend final : public playback_session::OpeningBackend {
 public:
  std::optional<playback_session::Problem>
  start(const playback_session::OpeningConfiguration& configuration) override {
    ++startCalls;
    lastConfiguration = configuration;
    return startFailure;
  }

  bool initializationDone() override { return initDone; }
  bool initializationSucceeded() const override { return initSucceeded; }
  std::string initializationError() const override { return initError; }
  bool finishInitialization() override {
    ++finishInitializationCalls;
    return initDone;
  }
  void requestClose() override { ++requestCloseCalls; }
  bool closeReady() override { return closingReady; }
  bool finishClose() override {
    if (!closingReady) return false;
    ++finishCloseCalls;
    return true;
  }
  std::vector<NativeWaitHandle> waitHandles() const override {
    return {NativeWaitHandle(reinterpret_cast<void*>(1))};
  }

  std::optional<playback_session::Problem> startFailure;
  playback_session::OpeningConfiguration lastConfiguration;
  std::string initError;
  bool initDone = false;
  bool initSucceeded = false;
  bool closingReady = false;
  int startCalls = 0;
  int requestCloseCalls = 0;
  int finishCloseCalls = 0;
  int finishInitializationCalls = 0;
};

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

PlaybackSessionBootstrap makeBootstrap(const std::filesystem::path& file,
                                       FakeOpeningBackend& backend) {
  fakeNowValue = wake_schedule::TimePoint{} + std::chrono::seconds(10);
  return PlaybackSessionBootstrap({file, true, false, backend, &fakeNow});
}

bool testAsyncReady() {
  FakeOpeningBackend backend;
  const std::filesystem::path file = L"C:\\media\\movie.mp4";
  PlaybackSessionBootstrap bootstrap = makeBootstrap(file, backend);

  bool ok = true;
  ok &= expect(!bootstrap.start(), "pending initialization stays asynchronous");
  ok &= expect(backend.startCalls == 1, "backend starts exactly once");
  ok &= expect(backend.lastConfiguration.file == file,
               "backend receives the owned media target");
  ok &= expect(backend.lastConfiguration.enableAudio,
               "backend receives audio capability");
  ok &= expect(!backend.lastConfiguration.allowDecoderScale,
               "backend receives presentation scaling policy");
  ok &= expect(!bootstrap.waitHandles().empty(),
               "pending initialization exposes its wake handle");
  ok &= expect(bootstrap.snapshot().stage ==
                   playback_session::TransitionStage::Opening,
               "pending initialization reports preparation");
  const wake_schedule::Deadline firstDraw = bootstrap.nextWakeDeadline();
  ok &= expect(firstDraw &&
                   *firstDraw == fakeNowValue + std::chrono::milliseconds(120),
               "preparation publishes a deterministic presentation deadline");
  fakeNowValue += std::chrono::milliseconds(800);
  ok &= expect(bootstrap.snapshot().activity > 0.9,
               "preparation animation is driven by the injected clock");

  backend.initDone = true;
  backend.initSucceeded = true;
  const std::optional<playback_session::OpenOutcome> outcome = bootstrap.pump();
  ok &= expect(
      outcome && std::holds_alternative<playback_session::OpenReady>(*outcome),
      "successful initialization resolves Ready");
  ok &= expect(bootstrap.waitHandles().empty(),
               "resolved initialization retires its wake handle");
  ok &= expect(backend.finishInitializationCalls == 1,
               "successful initialization is committed exactly once");
  ok &= expect(backend.requestCloseCalls == 0,
               "successful initialization leaves player running");
  return ok;
}

bool testReadyBeforeFirstWait() {
  FakeOpeningBackend backend;
  backend.initDone = true;
  backend.initSucceeded = true;
  PlaybackSessionBootstrap bootstrap =
      makeBootstrap(L"C:\\media\\movie.mp4", backend);

  const std::optional<playback_session::OpenOutcome> outcome =
      bootstrap.start();
  bool ok = true;
  ok &= expect(
      outcome && std::holds_alternative<playback_session::OpenReady>(*outcome),
      "completion published before the first wait is observed");
  ok &= expect(bootstrap.waitHandles().empty(),
               "immediate completion never leaves a stale wait handle");
  ok &= expect(backend.finishInitializationCalls == 1,
               "immediate initialization commits exactly once");
  return ok;
}

bool testFailureWaitsForAsyncClose() {
  FakeOpeningBackend backend;
  backend.initDone = true;
  backend.initError = "No video stream found in input.";
  PlaybackSessionBootstrap bootstrap =
      makeBootstrap(L"C:\\media\\audio-only.mkv", backend);

  bool ok = true;
  ok &= expect(!bootstrap.start(),
               "initialization failure remains pending during cleanup");
  ok &= expect(backend.requestCloseCalls == 1,
               "initialization failure requests cooperative close");
  ok &= expect(backend.finishCloseCalls == 0,
               "cleanup is not joined before its completion signal");
  ok &= expect(bootstrap.snapshot().stage ==
                   playback_session::TransitionStage::Closing,
               "failure cleanup has an explicit closing state");

  InputEvent escape{};
  escape.type = InputEvent::Type::Key;
  escape.key.vk = VK_ESCAPE;
  ok &= expect(bootstrap.handleInputEvent(escape),
               "Escape remains consumed while failed initialization closes");

  backend.closingReady = true;
  const std::optional<playback_session::OpenOutcome> outcome = bootstrap.pump();
  ok &= expect(
      outcome &&
          std::holds_alternative<playback_session::OpenAudioFallback>(*outcome),
      "cancel input cannot overwrite a committed failure outcome");
  ok &= expect(backend.finishCloseCalls == 1,
               "completed cleanup is reclaimed exactly once");
  return ok;
}

bool testQuitAlwaysOutranksCancel() {
  FakeOpeningBackend backend;
  PlaybackSessionBootstrap bootstrap =
      makeBootstrap(L"C:\\media\\movie.mp4", backend);
  bool ok = true;
  ok &= expect(!bootstrap.start(), "test opening starts pending");

  bootstrap.requestCancel();
  ok &= expect(backend.requestCloseCalls == 1,
               "cancel starts cooperative close once");
  ok &= expect(bootstrap.snapshot().stage ==
                   playback_session::TransitionStage::Cancelling,
               "cancel is visible while cleanup runs");
  InputEvent quit{};
  quit.type = InputEvent::Type::Key;
  quit.key.vk = 'Q';
  quit.key.ch = 'q';
  quit.key.control = LEFT_CTRL_PRESSED;
  ok &= expect(bootstrap.handleInputEvent(quit),
               "the global quit shortcut is consumed during preparation");
  bootstrap.requestCancel();
  ok &= expect(bootstrap.snapshot().stage ==
                   playback_session::TransitionStage::Closing,
               "quit upgrades and cannot be downgraded by cancel");

  backend.closingReady = true;
  const std::optional<playback_session::OpenOutcome> outcome = bootstrap.pump();
  ok &= expect(
      outcome && std::holds_alternative<playback_session::OpenQuitApplication>(
                     *outcome),
      "quit wins the cancel/quit race");
  ok &= expect(backend.requestCloseCalls == 1,
               "intent upgrades do not duplicate close requests");
  return ok;
}

bool testOpeningInputContract() {
  FakeOpeningBackend backend;
  PlaybackSessionBootstrap bootstrap =
      makeBootstrap(L"C:\\media\\movie.mp4", backend);
  bool ok = true;
  ok &= expect(!bootstrap.start(), "test opening starts pending");

  InputEvent unknown{};
  unknown.type = InputEvent::Type::Key;
  unknown.key.vk = 'A';
  unknown.key.ch = 'a';
  ok &= expect(!bootstrap.handleInputEvent(unknown),
               "unrelated input remains available to the shell");
  ok &= expect(backend.requestCloseCalls == 0,
               "unrelated input never cancels preparation");

  InputEvent resize{};
  resize.type = InputEvent::Type::Resize;
  ok &= expect(bootstrap.handleInputEvent(resize),
               "terminal resize invalidates opening presentation");
  ok &= expect(bootstrap.nextWakeDeadline() &&
                   *bootstrap.nextWakeDeadline() == fakeNowValue,
               "resize uses the injected owner clock for immediate redraw");
  ok &= expect(backend.requestCloseCalls == 0,
               "resize never changes the opening disposition");

  InputEvent escape{};
  escape.type = InputEvent::Type::Key;
  escape.key.vk = VK_ESCAPE;
  ok &= expect(bootstrap.handleInputEvent(escape),
               "Escape follows the standard cancel contract");
  ok &= expect(backend.requestCloseCalls == 1,
               "Escape initiates cooperative close exactly once");
  return ok;
}

bool testCancelCompletion() {
  FakeOpeningBackend backend;
  PlaybackSessionBootstrap bootstrap =
      makeBootstrap(L"C:\\media\\movie.mp4", backend);
  bool ok = true;
  ok &= expect(!bootstrap.start(), "test opening starts pending");
  bootstrap.requestCancel();
  backend.closingReady = true;
  const std::optional<playback_session::OpenOutcome> outcome = bootstrap.pump();
  ok &=
      expect(outcome && std::holds_alternative<playback_session::OpenCancelled>(
                            *outcome),
             "cancel resolves only after cooperative cleanup");
  return ok;
}

bool testImmediateStartFailure() {
  FakeOpeningBackend backend;
  backend.startFailure =
      playback_session::Problem{"Could not start decoder.", "test detail"};
  PlaybackSessionBootstrap bootstrap =
      makeBootstrap(L"C:\\media\\missing.mp4", backend);
  const std::optional<playback_session::OpenOutcome> outcome =
      bootstrap.start();
  bool ok = true;
  const auto* failure =
      outcome ? std::get_if<playback_session::OpenFailure>(&*outcome) : nullptr;
  ok &=
      expect(failure && failure->problem.message == "Could not start decoder.",
             "synchronous backend rejection preserves diagnostic context");
  ok &= expect(backend.requestCloseCalls == 0,
               "rejected starts do not close an unopened backend");
  return ok;
}

bool testOpeningViewCancelAffordance() {
  const tui_video_transition_view::Layout preparing =
      tui_video_transition_view::layout(
          80, 25, playback_session::TransitionStage::Opening);
  bool ok = true;
  ok &= expect(preparing.cancel.width == 10,
               "preparing view exposes a real Cancel target");

  InputEvent click{};
  click.type = InputEvent::Type::Mouse;
  click.mouse.kind = MouseEventKind::Press;
  click.mouse.button = MouseButton::Left;
  click.mouse.pos.X = static_cast<SHORT>(preparing.cancel.x + 1);
  click.mouse.pos.Y = static_cast<SHORT>(preparing.cancel.y);
  ok &= expect(tui_video_transition_view::cancelRequested(click, preparing),
               "left click activates the visible Cancel target");
  click.mouse.button = MouseButton::Right;
  ok &= expect(!tui_video_transition_view::cancelRequested(click, preparing),
               "undocumented right click never cancels preparation");

  const tui_video_transition_view::Layout closing =
      tui_video_transition_view::layout(
          80, 25, playback_session::TransitionStage::Closing);
  ok &= expect(closing.cancel.width == 0,
               "Cancel disappears after the close barrier is crossed");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testAsyncReady();
  ok &= testReadyBeforeFirstWait();
  ok &= testFailureWaitsForAsyncClose();
  ok &= testQuitAlwaysOutranksCancel();
  ok &= testOpeningInputContract();
  ok &= testCancelCompletion();
  ok &= testImmediateStartFailure();
  ok &= testOpeningViewCancelAffordance();
  if (!ok) return 1;
  std::cout << "playback_session_bootstrap_tests: OK\n";
  return 0;
}
