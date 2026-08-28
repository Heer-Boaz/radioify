#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>

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

InputEvent keyEvent(WORD key) {
  InputEvent event{};
  event.type = InputEvent::Type::Key;
  event.key.vk = key;
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

  return ok ? 0 : 1;
}
