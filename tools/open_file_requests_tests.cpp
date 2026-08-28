#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "core/open_file_requests.h"

#include <iostream>
#include <utility>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "open_file_requests_tests: " << message << '\n';
    return false;
  }
  return true;
}

}  // namespace

int main() {
  OpenFileRequests requests;
  OpenFilesRequest posted;
  posted.files.emplace_back("video.mp4");
  posted.presentation =
      OpenPresentationDirective::NativeWindowedFramebuffer;
  requests.post(std::move(posted));

  OpenFilesRequest received;
  bool ok = expect(requests.hasPending(),
                   "posted request must signal pending work");
  ok &= expect(requests.poll(received),
               "posted request must be available to the consumer");
  ok &= expect(received.files.size() == 1 &&
                   received.files.front() == "video.mp4",
               "request must preserve its file path");
  ok &= expect(received.presentation ==
                   OpenPresentationDirective::NativeWindowedFramebuffer,
               "request must preserve its framebuffer video intent");
  ok &= expect(!requests.poll(received),
               "poll must consume the queued request exactly once");
  ok &= expect(!requests.hasPending(),
               "consuming the queue must clear pending work");

  OpenFilesRequest firstQueued;
  firstQueued.files.emplace_back("first.mp4");
  OpenFilesRequest secondQueued;
  secondQueued.files.emplace_back("second.mp4");
  requests.post(std::move(firstQueued));
  requests.post(std::move(secondQueued));
  ok &= expect(requests.poll(received) &&
                   received.files.front() == "first.mp4" &&
                   requests.hasPending() &&
                   WaitForSingleObject(
                       static_cast<HANDLE>(requests.nativeWaitHandle().get()),
                       0) == WAIT_OBJECT_0,
               "polling one request must preserve FIFO order and keep the "
               "wake signal active for queued modal handoffs");
  ok &= expect(requests.poll(received) &&
                   received.files.front() == "second.mp4" &&
                   !requests.hasPending() &&
                   WaitForSingleObject(
                       static_cast<HANDLE>(requests.nativeWaitHandle().get()),
                       0) == WAIT_TIMEOUT,
               "the final queued request must clear the wake signal");

  OpenFilesRequest inherited;
  ok &= expect(inherited.presentation ==
                   OpenPresentationDirective::InheritActive,
               "in-process open requests must inherit the active video mode");
  return ok ? 0 : 1;
}
