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

MouseEvent classify(pointer_input::MouseDoubleClickTracker& tracker,
                    MouseEvent mouse, uint64_t timestampMs,
                    uint32_t maximumIntervalMs, int maximumDeltaX,
                    int maximumDeltaY) {
  tracker.classify(mouse, timestampMs, maximumIntervalMs, maximumDeltaX,
                   maximumDeltaY);
  return mouse;
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
                   firstPress.mouse.buttons == MouseButtons::Left &&
                   firstPress.mouse.button == MouseButton::Left &&
                   !firstPress.mouse.hasPixelPosition &&
                   firstPress.mouse.pos.X == 100 &&
                   firstPress.mouse.pos.Y == 80,
               "SGR mouse press must preserve its semantic transition and "
               "cell position");

  ok &= expect(parseSequence(parser, L"\x1b[<0;101;81m", firstRelease),
               "SGR mouse release must parse as an input event");
  ok &= expect(firstRelease.mouse.kind == MouseEventKind::Release &&
                   firstRelease.mouse.buttons == MouseButtons::None &&
                   firstRelease.mouse.button == MouseButton::Left,
               "SGR mouse release must not masquerade as a zero-button "
               "press");

  ok &= expect(parseSequence(parser, L"\x1b[<0;102;80M", secondPress),
               "A second SGR mouse press must parse as an input event");
  pointer_input::MouseDoubleClickTracker doubleClickTracker;
  ok &= expect(classify(doubleClickTracker, firstPress.mouse, 1000, 500, 2, 2)
                       .kind == MouseEventKind::Press,
               "A first terminal press must remain a single click");
  ok &= expect(classify(doubleClickTracker, firstRelease.mouse, 1020, 500, 2,
                        2)
                       .kind == MouseEventKind::Release,
               "A terminal release must arm double-click recognition");
  ok &= expect(classify(doubleClickTracker, secondPress.mouse, 1200, 500, 2, 2)
                       .kind == MouseEventKind::DoubleClick,
               "A nearby second terminal press within the configured interval "
               "must be recognized as a double-click by its surface owner");

  InputEvent drag{};
  ok &= expect(parseSequence(parser, L"\x1b[<32;120;90M", drag) &&
                   drag.mouse.kind == MouseEventKind::Move &&
                   drag.mouse.buttons == MouseButtons::Left &&
                   drag.mouse.button == MouseButton::None,
               "SGR drag reports must remain mouse moves with the left button "
               "held");

  InputEvent wheel{};
  ok &= expect(parseSequence(parser, L"\x1b[<64;120;90M", wheel) &&
                   wheel.mouse.kind == MouseEventKind::VerticalWheel &&
                   wheel.mouse.buttons == MouseButtons::None &&
                   wheel.mouse.button == MouseButton::None &&
                   wheel.mouse.wheelDelta == WHEEL_DELTA,
               "SGR wheel reports must use an explicit wheel delta");

  InputEvent middlePress{};
  ok &= expect(parseSequence(parser, L"\x1b[<1;120;90M", middlePress) &&
                   middlePress.mouse.kind == MouseEventKind::Press &&
                   middlePress.mouse.buttons == MouseButtons::Middle &&
                   middlePress.mouse.button == MouseButton::Middle,
               "SGR middle-button input must remain a pointer button rather "
               "than a browser-back command");

  doubleClickTracker.reset();
  classify(doubleClickTracker, firstPress.mouse, 2000, 500, 0, 0);
  classify(doubleClickTracker, firstRelease.mouse, 2020, 500, 0, 0);
  ok &= expect(classify(doubleClickTracker, secondPress.mouse, 2100, 500, 0, 0)
                       .kind == MouseEventKind::Press,
               "A second press outside the configured rectangle must start a "
               "new click");

  doubleClickTracker.reset();
  classify(doubleClickTracker, firstPress.mouse, 3000, 500, 2, 2);
  classify(doubleClickTracker, firstRelease.mouse, 3020, 500, 2, 2);
  ok &= expect(classify(doubleClickTracker, secondPress.mouse, 3600, 500, 2, 2)
                       .kind == MouseEventKind::Press,
               "A second press after the configured interval must remain a "
               "single click");

  pointer_input::MouseDoubleClickTracker playbackSurfaceTracker;
  classify(doubleClickTracker, firstPress.mouse, 4000, 500, 2, 2);
  classify(doubleClickTracker, firstRelease.mouse, 4020, 500, 2, 2);
  ok &= expect(classify(playbackSurfaceTracker, secondPress.mouse, 4100, 500,
                        2, 2)
                       .kind == MouseEventKind::Press,
               "clicks owned by different surfaces must never combine into "
               "a double-click");

  return ok ? 0 : 1;
}
