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
  posted.videoMode = OpenVideoMode::Framebuffer;
  requests.post(std::move(posted));

  OpenFilesRequest received;
  bool ok = expect(requests.hasPending(),
                   "posted request must signal pending work");
  ok &= expect(requests.poll(received),
               "posted request must be available to the consumer");
  ok &= expect(received.files.size() == 1 &&
                   received.files.front() == "video.mp4",
               "request must preserve its file path");
  ok &= expect(received.videoMode == OpenVideoMode::Framebuffer,
               "request must preserve its framebuffer video intent");
  ok &= expect(!requests.poll(received),
               "poll must consume the queued request exactly once");
  ok &= expect(!requests.hasPending(),
               "consuming the queue must clear pending work");

  OpenFilesRequest inherited;
  ok &= expect(inherited.videoMode == OpenVideoMode::Inherit,
               "in-process open requests must inherit the active video mode");
  return ok ? 0 : 1;
}
