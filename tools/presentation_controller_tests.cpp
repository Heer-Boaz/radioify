#include "playback/session/presentation_controller.h"

#include <cstdio>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::fprintf(stderr, "presentation_controller_tests: %s\n", message);
  return false;
}

class FakePresentationBackend final : public PlaybackPresentationBackend {
 public:
  PlaybackWindowLifecycle windowLifecycle() const override { return lifecycle; }

  bool consumeWindowLifecycleChange() override {
    const bool consumed = lifecycleChanged;
    lifecycleChanged = false;
    return consumed;
  }

  bool requestOpenWindow() override {
    ++openRequests;
    if (!acceptOpen) {
      lifecycle = PlaybackWindowLifecycle::Failed;
      lifecycleChanged = true;
      return false;
    }
    lifecycle = PlaybackWindowLifecycle::Opening;
    return true;
  }

  void requestCloseWindow() override {
    ++closeRequests;
    if (lifecycle != PlaybackWindowLifecycle::Closed) {
      lifecycle = PlaybackWindowLifecycle::Closing;
    }
  }

  bool windowCloseReady() const override { return closeReady; }

  bool finishCloseWindow() override {
    ++finishCloseCalls;
    if (!closeReady) return false;
    lifecycle = PlaybackWindowLifecycle::Closed;
    return true;
  }

  bool applyWindowPresentation(WindowPresentationRequest) override {
    ++applyCalls;
    return applySucceeds;
  }

  bool restoreWindowPresentation(WindowPresentationRequest,
                                 const WindowPlacementState&) override {
    ++restoreCalls;
    return restoreSucceeds;
  }

  bool captureWindowPlacement(WindowPlacementState&) override {
    ++captureCalls;
    return true;
  }

  void publishOpen() {
    lifecycle = PlaybackWindowLifecycle::Open;
    lifecycleChanged = true;
  }

  void publishOpenFailure() {
    lifecycle = PlaybackWindowLifecycle::Failed;
    lifecycleChanged = true;
  }

  PlaybackWindowLifecycle lifecycle = PlaybackWindowLifecycle::Closed;
  bool lifecycleChanged = false;
  bool acceptOpen = true;
  bool closeReady = false;
  bool applySucceeds = true;
  bool restoreSucceeds = true;
  int openRequests = 0;
  int closeRequests = 0;
  int finishCloseCalls = 0;
  int applyCalls = 0;
  int restoreCalls = 0;
  int captureCalls = 0;
};

bool settleTerminal(PlaybackPresentationController& controller,
                    FakePresentationBackend& backend) {
  const PlaybackPresentationSyncResult result = controller.synchronize(backend);
  return expect(!result.transitionPending && !result.transitionFailed &&
                    result.appliedState ==
                        PlaybackPresentationState::terminalAscii(),
                "initial terminal state must settle without a window");
}

bool testOpenIsPumpable() {
  bool ok = true;
  FakePresentationBackend backend;
  PlaybackPresentationController controller;
  ok &= settleTerminal(controller, backend);

  ok &= expect(controller.toggleWindow(),
               "window toggle must create a desired-state change");
  PlaybackPresentationSyncResult result = controller.synchronize(backend);
  ok &= expect(result.transitionPending && backend.openRequests == 1 &&
                   backend.restoreCalls == 0,
               "window creation must publish Pending without applying early");

  result = controller.synchronize(backend);
  ok &= expect(result.transitionPending && backend.openRequests == 1,
               "pumping an opening window must not duplicate the request");

  backend.publishOpen();
  result = controller.synchronize(backend);
  ok &= expect(!result.transitionPending && !result.transitionFailed &&
                   result.windowOpen && backend.restoreCalls == 1 &&
                   controller.state().requiresNativeWindow(),
               "the published open event must atomically settle presentation");
  return ok;
}

bool testCloseIsPumpable() {
  bool ok = true;
  FakePresentationBackend backend;
  PlaybackPresentationController controller;
  ok &= settleTerminal(controller, backend);
  controller.toggleWindow();
  (void)controller.synchronize(backend);
  backend.publishOpen();
  (void)controller.synchronize(backend);

  controller.toggleWindow();
  PlaybackPresentationSyncResult result = controller.synchronize(backend);
  ok &= expect(result.transitionPending && backend.closeRequests == 1 &&
                   backend.finishCloseCalls == 0 &&
                   controller.state().requiresNativeWindow(),
               "closing must retain the applied window state until exact exit");

  backend.closeReady = true;
  result = controller.synchronize(backend);
  ok &= expect(!result.transitionPending && result.switchedAwayFromWindow() &&
                   !controller.state().requiresNativeWindow() &&
                   backend.finishCloseCalls == 1,
               "exact window-thread exit must settle the terminal transition");
  return ok;
}

bool testOpeningCanBeCancelled() {
  bool ok = true;
  FakePresentationBackend backend;
  PlaybackPresentationController controller;
  ok &= settleTerminal(controller, backend);
  controller.toggleWindow();
  (void)controller.synchronize(backend);

  controller.toggleWindow();
  PlaybackPresentationSyncResult result = controller.synchronize(backend);
  ok &= expect(result.transitionPending && backend.closeRequests == 1 &&
                   backend.restoreCalls == 0,
               "cancelling an open must request close without applying it");

  backend.closeReady = true;
  result = controller.synchronize(backend);
  ok &= expect(!result.transitionPending && !result.transitionFailed &&
                   controller.state() ==
                       PlaybackPresentationState::terminalAscii(),
               "a cancelled open must return to the prior terminal state");
  return ok;
}

bool testOpenFailureWaitsForWorkerExit() {
  bool ok = true;
  FakePresentationBackend backend;
  PlaybackPresentationController controller;
  ok &= settleTerminal(controller, backend);
  controller.toggleWindow();
  (void)controller.synchronize(backend);

  backend.publishOpenFailure();
  PlaybackPresentationSyncResult result = controller.synchronize(backend);
  ok &= expect(result.transitionPending && !result.transitionFailed &&
                   backend.closeRequests == 1 && backend.finishCloseCalls == 0,
               "open failure must remain pending until its worker has exited");

  backend.closeReady = true;
  result = controller.synchronize(backend);
  ok &= expect(!result.transitionPending && result.transitionFailed &&
                   !result.appliedState.requiresNativeWindow() &&
                   backend.finishCloseCalls == 1,
               "failure must publish once after exact worker reclamation");
  return ok;
}

bool testInitialNativeFailureNeverClaimsWindowApplied() {
  bool ok = true;
  FakePresentationBackend backend;
  PlaybackPresentationController controller(
      PlaybackPresentationState::nativeWindowed());

  PlaybackPresentationSyncResult result = controller.synchronize(backend);
  ok &= expect(result.transitionPending &&
                   controller.state() ==
                       PlaybackPresentationState::terminalAscii(),
               "initial native open must not masquerade as already applied");

  backend.publishOpenFailure();
  result = controller.synchronize(backend);
  ok &= expect(result.transitionPending,
               "initial open failure must wait for exact worker exit");
  backend.closeReady = true;
  result = controller.synchronize(backend);
  ok &= expect(!result.transitionPending && result.transitionFailed &&
                   controller.state() ==
                       PlaybackPresentationState::terminalAscii() &&
                   !result.switchedAwayFromWindow(),
               "failed initial native presentation must remain terminal");
  return ok;
}

}  // namespace

int main() {
  bool ok = true;
  ok &= testOpenIsPumpable();
  ok &= testCloseIsPumpable();
  ok &= testOpeningCanBeCancelled();
  ok &= testOpenFailureWaitsForWorkerExit();
  ok &= testInitialNativeFailureNeverClaimsWindowApplied();
  if (!ok) return 1;
  std::puts("presentation_controller_tests: PASS");
  return 0;
}
