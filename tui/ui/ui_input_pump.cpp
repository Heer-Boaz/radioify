#include "ui_input_pump.h"

#include "playback/video/framebuffer/window/window.h"

namespace {

constexpr int kMaximumResizeEventsPerTurn = 32;

}  // namespace

bool ConsoleInputPump::pollNext(ConsoleInput& input, InputEvent& out) {
  if (queued_.has_value()) {
    out = *queued_;
    queued_.reset();
    return true;
  }

  if (!input.poll(out)) {
    return false;
  }
  if (out.type != InputEvent::Type::Resize) {
    return true;
  }

  InputEvent next{};
  for (int count = 1;
       count < kMaximumResizeEventsPerTurn && input.poll(next); ++count) {
    if (next.type == InputEvent::Type::Resize) {
      out = next;
      continue;
    }
    queued_ = next;
    break;
  }

  return true;
}

bool ApplicationInputPump::pollNext(ConsoleInput& console,
                                    VideoWindow* shellWindow, InputEvent& out,
                                    ApplicationInputSurface& source) {
  return pollNext(console, shellWindow, [](InputEvent&) { return false; }, out,
                  source);
}

bool ApplicationInputPump::pollShellWindow(VideoWindow* window,
                                            InputEvent& out) {
  return window && window->IsOpen() && window->PollInput(out);
}
