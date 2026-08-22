#include "core/shell_open_protocol.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "core/runtime_helpers.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "shell_open_protocol_tests: " << message << '\n';
    return false;
  }
  return true;
}

shell_open_protocol::Request requestWith(
    OpenPresentationDirective presentation) {
  shell_open_protocol::Request request;
  request.requestId = 0x123456789abcdef0ULL;
  request.openFiles.presentation = presentation;
  request.openFiles.files = {
      pathFromUtf8String("C:/Media/video.mp4"),
      pathFromUtf8String("C:/Media/caf\xC3\xA9/song.mp3")};
  return request;
}

}  // namespace

int main() {
  bool ok = true;
  const std::vector<OpenPresentationDirective> directives = {
      OpenPresentationDirective::InheritActive,
      OpenPresentationDirective::UseLaunchDefaults,
      OpenPresentationDirective::TerminalAscii,
      OpenPresentationDirective::NativeWindowedFramebuffer};
  for (OpenPresentationDirective directive : directives) {
    const shell_open_protocol::Request request = requestWith(directive);
    const auto encoded = shell_open_protocol::encodeRequest(request);
    const auto decoded = encoded
                             ? shell_open_protocol::decodeRequest(*encoded)
                             : shell_open_protocol::RequestDecodeResult{};
    ok &= expect(encoded && decoded.value,
                 "a valid request must round-trip");
    if (decoded.value) {
      ok &= expect(decoded.value->requestId == request.requestId,
                   "request id must round-trip");
      ok &= expect(decoded.value->openFiles.presentation == directive,
                   "presentation directive must round-trip");
      ok &= expect(decoded.value->openFiles.files == request.openFiles.files,
                   "all UTF-8 paths must round-trip");
    }
  }

  shell_open_protocol::Response response;
  response.requestId = 42;
  response.status = shell_open_protocol::ResponseStatus::Accepted;
  const auto decodedResponse = shell_open_protocol::decodeResponse(
      shell_open_protocol::encodeResponse(response));
  ok &= expect(decodedResponse.value &&
                   decodedResponse.value->requestId == 42 &&
                   decodedResponse.value->status ==
                       shell_open_protocol::ResponseStatus::Accepted,
               "an acknowledgement must round-trip");

  const auto valid = shell_open_protocol::encodeRequest(
      requestWith(OpenPresentationDirective::InheritActive));
  if (valid) {
    std::vector<std::uint8_t> invalidMagic = *valid;
    invalidMagic[0] = 'X';
    ok &= expect(shell_open_protocol::decodeRequest(invalidMagic).error ==
                     shell_open_protocol::DecodeError::InvalidMagic,
                 "an invalid magic must be rejected");

    std::vector<std::uint8_t> unsupportedVersion = *valid;
    unsupportedVersion[4] = 2;
    ok &= expect(shell_open_protocol::decodeRequest(unsupportedVersion).error ==
                     shell_open_protocol::DecodeError::UnsupportedVersion,
                 "an unsupported major version must be rejected");

    std::vector<std::uint8_t> invalidDirective = *valid;
    invalidDirective[32] = 0xff;
    ok &= expect(shell_open_protocol::decodeRequest(invalidDirective).error ==
                     shell_open_protocol::DecodeError::InvalidPayload,
                 "an unknown presentation directive must be rejected");

    std::vector<std::uint8_t> trailingBytes = *valid;
    trailingBytes.push_back(0);
    ok &= expect(shell_open_protocol::decodeRequest(trailingBytes).error ==
                     shell_open_protocol::DecodeError::InvalidHeader,
                 "trailing bytes must not be silently accepted");
  }

  shell_open_protocol::Request empty;
  empty.requestId = 1;
  ok &= expect(!shell_open_protocol::encodeRequest(empty),
               "an empty file request must not be encoded");

  shell_open_protocol::Request oversized;
  oversized.requestId = 2;
  oversized.openFiles.files.emplace_back(
      std::string(shell_open_protocol::kMaxMessageBytes, 'x'));
  ok &= expect(!shell_open_protocol::encodeRequest(oversized),
               "an oversized request must not be encoded");

  return ok ? 0 : 1;
}
