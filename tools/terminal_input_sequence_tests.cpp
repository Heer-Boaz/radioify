#include "input_event.h"
#include "mouse_double_click_tracker.h"
#include "terminal_input_sequence.h"

#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

bool parseSequence(TerminalInputSequenceParser& parser,
                   std::wstring_view sequence, InputEvent& event) {
  TerminalInputSequenceParser::Result result =
      TerminalInputSequenceParser::Result::None;
  for (wchar_t ch : sequence) {
    result = parser.feed(ch, event);
  }
  return result == TerminalInputSequenceParser::Result::Event;
}

terminal_input::MouseClickTransition transitionFor(const MouseEvent& mouse) {
  switch (mouse.kind) {
    case MouseEventKind::Press:
      return terminal_input::MouseClickTransition::Press;
    case MouseEventKind::Release:
      return terminal_input::MouseClickTransition::Release;
    case MouseEventKind::Move:
      return terminal_input::MouseClickTransition::Move;
    default:
      return terminal_input::MouseClickTransition::Move;
  }
}

bool observe(terminal_input::MouseDoubleClickTracker& tracker,
             const MouseEvent& mouse, uint64_t timestampMs,
             uint32_t maximumIntervalMs, int maximumDeltaX,
             int maximumDeltaY) {
  return tracker.observe(transitionFor(mouse), mouse.pos.X, mouse.pos.Y,
                         timestampMs, maximumIntervalMs, maximumDeltaX,
                         maximumDeltaY);
}

}  // namespace

int main() {
  bool ok = true;
  TerminalInputSequenceParser parser;
  InputEvent firstPress{};
  InputEvent firstRelease{};
  InputEvent secondPress{};

  ok &= expect(parseSequence(parser, L"\x1b[<0;101;81M", firstPress),
               "SGR mouse press must parse as an input event");
  ok &= expect(firstPress.type == InputEvent::Type::Mouse &&
                   firstPress.mouse.kind == MouseEventKind::Press &&
                   firstPress.mouse.buttonState ==
                       FROM_LEFT_1ST_BUTTON_PRESSED &&
                   !firstPress.mouse.hasPixelPosition &&
                   firstPress.mouse.pos.X == 100 &&
                   firstPress.mouse.pos.Y == 80,
               "SGR mouse press must preserve its semantic transition and "
               "cell position");

  ok &= expect(parseSequence(parser, L"\x1b[<0;101;81m", firstRelease),
               "SGR mouse release must parse as an input event");
  ok &= expect(firstRelease.mouse.kind == MouseEventKind::Release &&
                   firstRelease.mouse.buttonState == 0,
               "SGR mouse release must not masquerade as a zero-button "
               "press");

  ok &= expect(parseSequence(parser, L"\x1b[<0;102;80M", secondPress),
               "A second SGR mouse press must parse as an input event");
  terminal_input::MouseDoubleClickTracker doubleClickTracker;
  ok &= expect(!observe(doubleClickTracker, firstPress.mouse, 1000, 500, 2, 2),
               "A first terminal press must remain a single click");
  ok &= expect(
      !observe(doubleClickTracker, firstRelease.mouse, 1020, 500, 2, 2),
      "A terminal release must arm double-click recognition");
  ok &= expect(observe(doubleClickTracker, secondPress.mouse, 1200, 500, 2, 2),
               "A nearby second terminal press within the configured interval "
               "must be recognized as a double-click");

  InputEvent drag{};
  ok &= expect(parseSequence(parser, L"\x1b[<32;120;90M", drag) &&
                   drag.mouse.kind == MouseEventKind::Move &&
                   drag.mouse.buttonState ==
                       FROM_LEFT_1ST_BUTTON_PRESSED,
               "SGR drag reports must remain mouse moves with the left button "
               "held");

  InputEvent wheel{};
  ok &= expect(parseSequence(parser, L"\x1b[<64;120;90M", wheel) &&
                   wheel.mouse.kind == MouseEventKind::VerticalWheel &&
                   wheel.mouse.buttonState == 0 &&
                   wheel.mouse.wheelDelta == WHEEL_DELTA,
               "SGR wheel reports must use an explicit wheel delta");

  doubleClickTracker.reset();
  observe(doubleClickTracker, firstPress.mouse, 2000, 500, 0, 0);
  observe(doubleClickTracker, firstRelease.mouse, 2020, 500, 0, 0);
  ok &= expect(!observe(doubleClickTracker, secondPress.mouse, 2100, 500, 0,
                        0),
               "A second press outside the configured rectangle must start a "
               "new click");

  doubleClickTracker.reset();
  observe(doubleClickTracker, firstPress.mouse, 3000, 500, 2, 2);
  observe(doubleClickTracker, firstRelease.mouse, 3020, 500, 2, 2);
  ok &= expect(!observe(doubleClickTracker, secondPress.mouse, 3600, 500, 2,
                        2),
               "A second press after the configured interval must remain a "
               "single click");

  return ok ? 0 : 1;
}
