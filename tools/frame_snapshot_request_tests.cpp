#include "playback/framebuffer/frame_snapshot_request.h"

#include <iostream>
#include <thread>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "frame_snapshot_request_tests: " << message << '\n';
    return false;
  }
  return true;
}

VideoFrameSnapshotResult successfulSnapshot() {
  VideoFrameSnapshotResult result;
  result.snapshot.width = 1;
  result.snapshot.height = 1;
  result.snapshot.strideBytes = 4;
  result.snapshot.rgba = {1, 2, 3, 255};
  return result;
}

}  // namespace

int main() {
  bool ok = true;
  playback_framebuffer_presenter::FrameSnapshotRequest request;

  ok &= expect(!request.pending(),
               "a new request must expose an idle fast-path");
  ok &= expect(request.begin(), "the idle request must accept capture work");
  ok &= expect(request.pending(),
               "accepted capture work must be visible to the presenter");
  ok &= expect(!request.begin(),
               "a second capture must not replace in-flight work");

  std::thread completion(
      [&request]() { request.complete(successfulSnapshot()); });
  VideoFrameSnapshotResult result = request.wait();
  completion.join();
  ok &= expect(result.succeeded(),
               "the waiting caller must receive the presenter result");
  ok &= expect(!request.pending(),
               "completed work must leave the presenter idle");

  ok &= expect(request.begin(), "a completed request must be reusable");
  request.cancel("window stopped");
  request.complete(successfulSnapshot());
  ok &= expect(!request.pending(),
               "cancelled work must no longer remain pending");
  result = request.wait();
  ok &= expect(!result.succeeded() && result.error == "window stopped",
               "cancellation must win over a late presenter completion");

  return ok ? 0 : 1;
}
