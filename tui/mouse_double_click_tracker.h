#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "input_event.h"

namespace pointer_input {

// Terminal protocols report pointer transitions without assigning a gesture
// to a widget. Each interaction surface owns an instance so clicks cannot be
// combined across the browser and playback surfaces.
class MouseDoubleClickTracker {
 public:
  // Use the platform gesture policy while keeping the timestamp/threshold
  // overload below available for deterministic tests.
  void classifyUsingSystemSettings(MouseEvent& mouse, double unitPixelWidth,
                                   double unitPixelHeight) {
    const int maximumDeltaX = static_cast<int>(
        std::max(0, GetSystemMetrics(SM_CXDOUBLECLK) / 2) /
        std::max(1.0, unitPixelWidth));
    const int maximumDeltaY = static_cast<int>(
        std::max(0, GetSystemMetrics(SM_CYDOUBLECLK) / 2) /
        std::max(1.0, unitPixelHeight));
    classify(mouse, GetTickCount64(), GetDoubleClickTime(), maximumDeltaX,
             maximumDeltaY);
  }

  void classify(MouseEvent& mouse, uint64_t timestampMs,
                uint32_t maximumIntervalMs, int maximumDeltaX,
                int maximumDeltaY) {
    if (isWindowMouseEvent(mouse)) {
      reset();
      return;
    }
    if (mouse.kind == MouseEventKind::DoubleClick) {
      reset();
      return;
    }

    const bool leftPressed = isMouseButtonDown(mouse, MouseButton::Left);
    Transition transition;
    if (mouse.kind == MouseEventKind::Move && leftPressed) {
      transition = Transition::Move;
    } else if (mouse.kind == MouseEventKind::Press &&
               mouse.button == MouseButton::Left) {
      transition = Transition::Press;
    } else if (mouse.kind == MouseEventKind::Release &&
               mouse.button == MouseButton::Left) {
      transition = Transition::Release;
    } else {
      if (mouse.kind != MouseEventKind::Move) reset();
      return;
    }

    maximumDeltaX = std::max(0, maximumDeltaX);
    maximumDeltaY = std::max(0, maximumDeltaY);
    if (phase_ != Phase::Idle &&
        (timestampMs < firstPressTimestampMs_ ||
         timestampMs - firstPressTimestampMs_ > maximumIntervalMs)) {
      reset();
    }

    switch (transition) {
      case Transition::Press:
        if (phase_ == Phase::AwaitingSecondPress &&
            sameLocation(mouse.pos.X, mouse.pos.Y, maximumDeltaX,
                         maximumDeltaY)) {
          reset();
          mouse.kind = MouseEventKind::DoubleClick;
          return;
        }
        beginFirstPress(mouse.pos.X, mouse.pos.Y, timestampMs);
        return;

      case Transition::Release:
        if (phase_ == Phase::FirstButtonDown) {
          phase_ = sameLocation(mouse.pos.X, mouse.pos.Y, maximumDeltaX,
                                maximumDeltaY)
                       ? Phase::AwaitingSecondPress
                       : Phase::Idle;
        }
        return;

      case Transition::Move:
        if (phase_ == Phase::FirstButtonDown &&
            !sameLocation(mouse.pos.X, mouse.pos.Y, maximumDeltaX,
                          maximumDeltaY)) {
          reset();
        }
        return;
    }
  }

  void reset() {
    phase_ = Phase::Idle;
    firstPressTimestampMs_ = 0;
  }

 private:
  enum class Transition : uint8_t {
    Press,
    Release,
    Move,
  };

  enum class Phase : uint8_t {
    Idle,
    FirstButtonDown,
    AwaitingSecondPress,
  };

  void beginFirstPress(int x, int y, uint64_t timestampMs) {
    phase_ = Phase::FirstButtonDown;
    firstPressX_ = x;
    firstPressY_ = y;
    firstPressTimestampMs_ = timestampMs;
  }

  bool sameLocation(int x, int y, int maximumDeltaX,
                    int maximumDeltaY) const {
    return std::abs(x - firstPressX_) <= maximumDeltaX &&
           std::abs(y - firstPressY_) <= maximumDeltaY;
  }

  Phase phase_ = Phase::Idle;
  int firstPressX_ = 0;
  int firstPressY_ = 0;
  uint64_t firstPressTimestampMs_ = 0;
};

}  // namespace pointer_input
