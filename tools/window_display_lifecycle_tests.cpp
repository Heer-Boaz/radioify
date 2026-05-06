#include "playback/video/framebuffer/window/display_lifecycle.h"

#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "window_display_lifecycle_tests: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  bool ok = true;

  WindowDisplayLifecycle display;
  WindowDisplayLifecycle::Work work;
  ok &= expect(!display.consume(work),
               "empty lifecycle must not produce display work");

  display.clientResized(640, 480);
  ok &= expect(display.consume(work), "client resize must be consumable");
  ok &= expect(work.kind == WindowDisplayLifecycle::WorkKind::ClientResize,
               "client resize must keep its work kind");
  ok &= expect(work.width == 640 && work.height == 480,
               "client resize must preserve dimensions");
  ok &= expect(!display.consume(work),
               "consumed client resize must clear pending work");

  display.clientResized(800, 600);
  display.clientResized(1024, 768);
  ok &= expect(display.consume(work),
               "coalesced client resize must be consumable");
  ok &= expect(work.kind == WindowDisplayLifecycle::WorkKind::ClientResize,
               "coalesced client resize must remain a resize");
  ok &= expect(work.width == 1024 && work.height == 768,
               "coalesced client resize must keep latest dimensions");

  display.clientResized(1280, 720);
  display.displayChanged(1920, 1080);
  display.clientResized(1600, 900);
  ok &= expect(display.consume(work),
               "display change must be consumable after resize traffic");
  ok &= expect(work.kind == WindowDisplayLifecycle::WorkKind::DisplayChange,
               "display change must take priority over resize traffic");
  ok &= expect(work.width == 1600 && work.height == 900,
               "display change must keep latest client dimensions");

  display.clientResized(0, 600);
  display.displayChanged(1920, 0);
  ok &= expect(!display.consume(work),
               "invalid display dimensions must not produce work");

  display.displayChanged(1920, 1080);
  {
    auto transition = display.ownerTransition();
    ok &= expect(!display.consume(work),
                 "owner transition must supersede older pending work");
    display.clientResized(300, 200);
    display.displayChanged(300, 200);
    ok &= expect(!display.consume(work),
                 "owner transition must defer pending display work");
  }
  ok &= expect(display.consume(work),
               "owner transition must preserve deferred display work");
  ok &= expect(work.kind == WindowDisplayLifecycle::WorkKind::DisplayChange,
               "deferred display work must keep display-change priority");
  ok &= expect(work.width == 300 && work.height == 200,
               "deferred display work must keep latest dimensions");

  display.clientResized(500, 400);
  ok &= expect(display.consume(work),
               "lifecycle must accept external work after owner transition");
  ok &= expect(work.width == 500 && work.height == 400,
               "external work after owner transition must keep dimensions");

  return ok ? 0 : 1;
}
