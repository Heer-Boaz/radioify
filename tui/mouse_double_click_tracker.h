#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace terminal_input {

enum class MouseClickTransition : uint8_t {
  Press,
  Release,
  Move,
};

// Recognizes the second press of a double-click for terminal protocols that
// report only pointer transitions. Timing and distance limits are supplied by
// the platform adapter so this state machine remains deterministic in tests.
class MouseDoubleClickTracker {
 public:
  bool observe(MouseClickTransition transition, int x, int y,
               bool pixelCoordinates, uint64_t timestampMs,
               uint32_t maximumIntervalMs, int maximumDeltaX,
               int maximumDeltaY) {
    maximumDeltaX = std::max(0, maximumDeltaX);
    maximumDeltaY = std::max(0, maximumDeltaY);
    if (phase_ != Phase::Idle &&
        (timestampMs < firstPressTimestampMs_ ||
         timestampMs - firstPressTimestampMs_ > maximumIntervalMs)) {
      reset();
    }

    switch (transition) {
      case MouseClickTransition::Press:
        if (phase_ == Phase::AwaitingSecondPress &&
            sameLocation(x, y, pixelCoordinates, maximumDeltaX,
                         maximumDeltaY)) {
          reset();
          return true;
        }
        beginFirstPress(x, y, pixelCoordinates, timestampMs);
        return false;

      case MouseClickTransition::Release:
        if (phase_ == Phase::FirstButtonDown) {
          phase_ = sameLocation(x, y, pixelCoordinates, maximumDeltaX,
                                maximumDeltaY)
                       ? Phase::AwaitingSecondPress
                       : Phase::Idle;
        }
        return false;

      case MouseClickTransition::Move:
        if (phase_ == Phase::FirstButtonDown &&
            !sameLocation(x, y, pixelCoordinates, maximumDeltaX,
                          maximumDeltaY)) {
          reset();
        }
        return false;
    }
    return false;
  }

  void reset() {
    phase_ = Phase::Idle;
    firstPressTimestampMs_ = 0;
  }

 private:
  enum class Phase : uint8_t {
    Idle,
    FirstButtonDown,
    AwaitingSecondPress,
  };

  void beginFirstPress(int x, int y, bool pixelCoordinates,
                       uint64_t timestampMs) {
    phase_ = Phase::FirstButtonDown;
    firstPressX_ = x;
    firstPressY_ = y;
    firstPressUsesPixels_ = pixelCoordinates;
    firstPressTimestampMs_ = timestampMs;
  }

  bool sameLocation(int x, int y, bool pixelCoordinates, int maximumDeltaX,
                    int maximumDeltaY) const {
    return pixelCoordinates == firstPressUsesPixels_ &&
           std::abs(x - firstPressX_) <= maximumDeltaX &&
           std::abs(y - firstPressY_) <= maximumDeltaY;
  }

  Phase phase_ = Phase::Idle;
  int firstPressX_ = 0;
  int firstPressY_ = 0;
  bool firstPressUsesPixels_ = false;
  uint64_t firstPressTimestampMs_ = 0;
};

}  // namespace terminal_input
