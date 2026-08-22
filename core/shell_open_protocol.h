#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "open_file_requests.h"

namespace shell_open_protocol {

constexpr std::size_t kMaxMessageBytes = 64u * 1024u;
constexpr std::size_t kMaxFileCount = 64u;

// All integers are little-endian. A 32-byte envelope carries the magic
// "RSHO", major/minor version, message type, header/payload sizes, request id,
// and reserved fields. Request payloads contain a presentation directive and
// length-prefixed UTF-8 paths; responses contain a ResponseStatus.

enum class DecodeError : std::uint8_t {
  None,
  Truncated,
  InvalidMagic,
  UnsupportedVersion,
  InvalidHeader,
  InvalidMessageType,
  InvalidPayload,
};

enum class ResponseStatus : std::uint32_t {
  Accepted = 0,
  InvalidRequest = 1,
  UnsupportedVersion = 2,
  InternalError = 3,
};

struct Request {
  std::uint64_t requestId = 0;
  OpenFilesRequest openFiles;
};

struct Response {
  std::uint64_t requestId = 0;
  ResponseStatus status = ResponseStatus::InvalidRequest;
};

struct RequestDecodeResult {
  std::optional<Request> value;
  DecodeError error = DecodeError::None;
};

struct ResponseDecodeResult {
  std::optional<Response> value;
  DecodeError error = DecodeError::None;
};

std::optional<std::vector<std::uint8_t>> encodeRequest(
    const Request& request);
RequestDecodeResult decodeRequest(const std::vector<std::uint8_t>& message);

std::vector<std::uint8_t> encodeResponse(const Response& response);
ResponseDecodeResult decodeResponse(const std::vector<std::uint8_t>& message);

}  // namespace shell_open_protocol
