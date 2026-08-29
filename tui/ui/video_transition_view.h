#pragma once

#include <algorithm>

#include "playback/session/transition_state.h"
#include "tui/input_event.h"
#include "tui/style.h"

class ConsoleScreen;

namespace tui_video_transition_view {

struct Styles {
  Style base;
  Style accent;
  Style dim;
  Style progressEmpty;
  Style progressFrame;
  Color progressStart;
  Color progressEnd;
};

struct ButtonBounds {
  int x = 0;
  int y = -1;
  int width = 0;

  bool contains(int pointerX, int pointerY) const {
    return width > 0 && pointerY == y && pointerX >= x && pointerX < x + width;
  }
};

struct Layout {
  ButtonBounds cancel;
};

inline Layout layout(int width, int height,
                     playback_session::TransitionStage stage) {
  Layout result;
  constexpr int kCancelWidth = 10;
  if (stage != playback_session::TransitionStage::Opening ||
      width < kCancelWidth || height <= 0) {
    return result;
  }
  const int messageY = std::clamp(height / 2 - 1, 1, std::max(1, height - 1));
  const int cancelY = messageY + 3;
  if (cancelY < height) {
    result.cancel = {(width - kCancelWidth) / 2, cancelY, kCancelWidth};
  }
  return result;
}

inline bool cancelRequested(const InputEvent& event,
                            const Layout& currentLayout) {
  return event.type == InputEvent::Type::Mouse &&
         event.mouse.kind == MouseEventKind::Press &&
         event.mouse.button == MouseButton::Left &&
         currentLayout.cancel.contains(event.mouse.pos.X, event.mouse.pos.Y);
}

Layout draw(ConsoleScreen& screen,
            const playback_session::TransitionSnapshot& snapshot,
            const Styles& styles);

}  // namespace tui_video_transition_view
