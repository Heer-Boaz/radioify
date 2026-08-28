#include "core/windows_file_drop_acceptance.h"

#include <cstdio>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::fprintf(stderr, "windows_file_drop_acceptance_tests: %s\n", message);
  return false;
}

}  // namespace

int main() {
  using windows_file_drop::AcceptanceState;
  bool ok = true;
  AcceptanceState state;

  AcceptanceState::Transition transition = state.update(true);
  ok &= expect(transition.accept && transition.beginHover &&
                   !transition.cancelHover,
               "an enabled target must begin a valid hover exactly once");
  transition = state.update(true);
  ok &= expect(transition.accept && !transition.beginHover &&
                   !transition.cancelHover,
               "drag-over must preserve an existing valid hover");
  ok &= expect(state.setEnabled(false),
               "disabling a hovered target must request hover cancellation");
  transition = state.complete(true);
  ok &= expect(!transition.accept && !transition.beginHover &&
                   !transition.cancelHover,
               "a disabled target must reject drop without double-cancel");
  ok &= expect(!state.setEnabled(false),
               "repeated disable must be idempotent");

  state.setEnabled(true);
  transition = state.update(true);
  ok &= expect(transition.accept && transition.beginHover,
               "re-enabling must permit a fresh hover");
  transition = state.complete(false);
  ok &= expect(!transition.accept && transition.cancelHover,
               "an invalid final source effect must cancel instead of drop");

  transition = state.update(true);
  ok &= expect(transition.accept && transition.beginHover && state.leave(),
               "drag-leave must end the active hover");
  ok &= expect(!state.leave(),
               "drag-leave cancellation must be emitted at most once");
  return ok ? 0 : 1;
}
