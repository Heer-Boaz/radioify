#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <vector>

#include "playback/input/shortcuts.h"
#include "playback/video/framebuffer/window/input_queue.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "window_input_queue_tests: %s\n", message);
    return false;
  }
  return true;
}

DWORD waitNow(const WindowInputQueue& queue) {
  return WaitForSingleObject(
      static_cast<HANDLE>(queue.nativeWaitHandle().get()), 0);
}

InputEvent keyEvent(WORD key,
                    KeyPressKind pressKind = KeyPressKind::Initial,
                    std::uint32_t repeatCount = 1) {
  InputEvent event{};
  event.type = InputEvent::Type::Key;
  event.key.vk = key;
  event.key.pressKind = pressKind;
  event.key.repeatCount = repeatCount;
  return event;
}

InputEvent mouseMoveEvent(SHORT x, MouseButtons buttons) {
  InputEvent event{};
  event.type = InputEvent::Type::Mouse;
  event.mouse.pos.X = x;
  event.mouse.buttons = buttons;
  event.mouse.kind = MouseEventKind::Move;
  return event;
}

InputEvent pointerLeaveEvent() {
  InputEvent event{};
  event.type = InputEvent::Type::PointerLeave;
  return event;
}

InputEvent resizeEvent(SHORT width, SHORT height) {
  InputEvent event{};
  event.type = InputEvent::Type::Resize;
  event.size = COORD{width, height};
  return event;
}

}  // namespace

int main() {
  bool ok = true;
  WindowInputQueue queue;
  InputEvent event{};

  ok &= expect(waitNow(queue) == WAIT_TIMEOUT,
               "new queue must start without a wake signal");

  queue.push(keyEvent('A'));
  queue.push(keyEvent('B'));
  ok &= expect(waitNow(queue) == WAIT_OBJECT_0,
               "pushing input must wake the playback loop");

  ok &= expect(queue.poll(event) && event.key.vk == 'A',
               "queue must preserve input order");
  ok &= expect(waitNow(queue) == WAIT_OBJECT_0,
               "wake signal must remain set while input is queued");

  ok &= expect(queue.poll(event) && event.key.vk == 'B',
               "queue must return the final input event");
  ok &= expect(waitNow(queue) == WAIT_TIMEOUT,
               "draining the queue must reset the wake signal");

  queue.push(keyEvent('C'));
  queue.clear();
  ok &= expect(waitNow(queue) == WAIT_TIMEOUT,
               "clearing the queue must reset the wake signal");
  ok &= expect(!queue.poll(event), "cleared queue must be empty");

  queue.push(mouseMoveEvent(10, MouseButtons::Left));
  queue.push(mouseMoveEvent(20, MouseButtons::Left));
  ok &= expect(queue.poll(event) && event.mouse.pos.X == 20,
               "adjacent drag moves must coalesce to the newest position");
  ok &= expect(!queue.poll(event),
               "coalesced drag moves must occupy one queue entry");

  queue.push(mouseMoveEvent(30, MouseButtons::Left));
  queue.push(mouseMoveEvent(40, MouseButtons::None));
  ok &= expect(queue.poll(event) &&
                   event.mouse.buttons == MouseButtons::Left,
               "mouse button transitions must preserve the final drag move");
  ok &= expect(queue.poll(event) &&
                   event.mouse.buttons == MouseButtons::None,
               "mouse button transitions must remain distinct events");

  queue.push(mouseMoveEvent(50, MouseButtons::None));
  queue.push(pointerLeaveEvent());
  ok &= expect(queue.poll(event) && event.type == InputEvent::Type::Mouse,
               "the final pointer position must precede pointer leave");
  ok &= expect(queue.poll(event) &&
                   event.type == InputEvent::Type::PointerLeave,
               "pointer leave must be an explicit non-sentinel event");

  queue.push(resizeEvent(80, 25));
  queue.push(resizeEvent(120, 40));
  ok &= expect(queue.poll(event) && event.type == InputEvent::Type::Resize &&
                   event.size.X == 120 && event.size.Y == 40,
               "adjacent native resize events must coalesce to the latest "
               "geometry");
  ok &= expect(!queue.poll(event),
               "coalesced native resize events must occupy one queue entry");

  queue.push(keyEvent(VK_SPACE));
  for (int repeat = 0; repeat < 100; ++repeat) {
    queue.push(keyEvent(VK_SPACE, KeyPressKind::AutoRepeat));
  }
  queue.push(keyEvent(kPlaybackVkMediaPlay));
  std::vector<PlaybackAction> actions;
  int deliveredEvents = 0;
  std::uint32_t spaceRepeatCount = 0;
  while (queue.poll(event)) {
    ++deliveredEvents;
    if (event.type == InputEvent::Type::Key &&
        event.key.vk == VK_SPACE && isAutoRepeat(event.key)) {
      spaceRepeatCount = keyPressCount(event.key);
    }
    if (const auto action = resolvePlaybackAction(event.key)) {
      actions.push_back(*action);
    }
  }
  ok &= expect(deliveredEvents == 3,
               "an auto-repeat burst must occupy one bounded queue slot");
  ok &= expect(spaceRepeatCount == 100,
               "coalescing must retain the repeat multiplicity instead of "
               "expanding or discarding Windows ticks");
  ok &= expect(actions.size() == 2 &&
                   actions[0] == PlaybackAction::TogglePause &&
                   actions[1] == PlaybackAction::Play,
               "a repeat flood must not delay or duplicate the following "
               "resume command");

  queue.push(keyEvent(VK_OEM_4));
  for (int repeat = 0; repeat < 20; ++repeat) {
    queue.push(keyEvent(VK_OEM_4, KeyPressKind::AutoRepeat));
  }
  actions.clear();
  std::uint32_t seekTicks = 0;
  while (queue.poll(event)) {
    if (const auto action = resolvePlaybackAction(event.key)) {
      actions.push_back(*action);
      const std::optional<PlaybackActionMatch> match =
          resolvePlaybackActionMatch(event);
      if (match) {
        const playback_input::Command command =
            playback_input::commandForShortcut(*match);
        if (const auto* seek =
                std::get_if<playback_input::SeekBySteps>(&command)) {
          seekTicks += static_cast<std::uint32_t>(
              seek->steps < 0 ? -seek->steps : seek->steps);
        }
      }
    }
  }
  ok &= expect(actions.size() == 2 &&
                   actions[0] == PlaybackAction::SeekBackward &&
                   actions[1] == PlaybackAction::SeekBackward,
               "coalescing must preserve one current repeat for a "
               "repeatable seek action");
  ok &= expect(seekTicks == 21,
               "a counted seek burst must preserve every logical tick in "
               "two bounded events");

  for (int repeat = 0; repeat < 1000; ++repeat) {
    const WORD key = (repeat % 2) == 0 ? VK_LEFT : VK_RIGHT;
    queue.push(keyEvent(key, KeyPressKind::AutoRepeat, 2));
    queue.push(mouseMoveEvent(static_cast<SHORT>(repeat),
                              MouseButtons::None));
    queue.push(resizeEvent(static_cast<SHORT>(80 + repeat % 20), 25));
  }
  queue.push(keyEvent(kPlaybackVkMediaPlay));
  int mixedStateEvents = 0;
  WORD latestRepeatedKey = 0;
  while (queue.poll(event)) {
    ++mixedStateEvents;
    if (event.type == InputEvent::Type::Key && isAutoRepeat(event.key)) {
      latestRepeatedKey = event.key.vk;
    }
  }
  ok &= expect(mixedStateEvents <= 4 && latestRepeatedKey == VK_RIGHT,
               "alternating repeats interleaved with mouse and resize state "
               "must retain only current intent, not an unbounded backlog");

  for (std::size_t index = 0;
       index < WindowInputQueue::kMaximumPendingEvents * 2; ++index) {
    queue.push(keyEvent(static_cast<WORD>('A' + index % 20)));
  }
  queue.push(keyEvent(kPlaybackVkMediaPlay));
  std::size_t boundedCount = 0;
  WORD finalKey = 0;
  while (queue.poll(event)) {
    ++boundedCount;
    finalKey = event.key.vk;
  }
  ok &= expect(
      boundedCount == WindowInputQueue::kMaximumPendingEvents &&
          finalKey == kPlaybackVkMediaPlay,
      "an all-discrete producer flood must stay capacity-bounded and retain "
      "the newest explicit command");

  return ok ? 0 : 1;
}
