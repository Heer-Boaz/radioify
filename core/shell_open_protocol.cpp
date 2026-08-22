#include "shell_open_protocol.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#include "runtime_helpers.h"
#include "utf8.h"

namespace shell_open_protocol {
namespace {

constexpr std::array<std::uint8_t, 4> kMagic = {'R', 'S', 'H', 'O'};
constexpr std::uint16_t kProtocolMajor = 1;
constexpr std::uint16_t kProtocolMinor = 0;
constexpr std::uint16_t kHeaderSize = 32;

enum class MessageType : std::uint16_t {
  OpenFiles = 1,
  Response = 2,
};

class Writer {
 public:
  void writeU8(std::uint8_t value) { bytes_.push_back(value); }

  void writeU16(std::uint16_t value) {
    writeU8(static_cast<std::uint8_t>(value));
    writeU8(static_cast<std::uint8_t>(value >> 8));
  }

  void writeU32(std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
      writeU8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void writeU64(std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
      writeU8(static_cast<std::uint8_t>(value >> shift));
    }
  }

  void writeBytes(std::string_view value) {
    bytes_.insert(bytes_.end(), value.begin(), value.end());
  }

  void writeMagic() { bytes_.insert(bytes_.end(), kMagic.begin(), kMagic.end()); }

  std::vector<std::uint8_t> take() { return std::move(bytes_); }

 private:
  std::vector<std::uint8_t> bytes_;
};

class Reader {
 public:
  Reader(const std::vector<std::uint8_t>& bytes, std::size_t begin,
         std::size_t end)
      : bytes_(bytes), cursor_(begin), end_(end) {}

  bool readU8(std::uint8_t& value) {
    if (cursor_ >= end_) return false;
    value = bytes_[cursor_++];
    return true;
  }

  bool readU16(std::uint16_t& value) {
    std::uint8_t low = 0;
    std::uint8_t high = 0;
    if (!readU8(low) || !readU8(high)) return false;
    value = static_cast<std::uint16_t>(low) |
            (static_cast<std::uint16_t>(high) << 8);
    return true;
  }

  bool readU32(std::uint32_t& value) {
    value = 0;
    for (int shift = 0; shift < 32; shift += 8) {
      std::uint8_t byte = 0;
      if (!readU8(byte)) return false;
      value |= static_cast<std::uint32_t>(byte) << shift;
    }
    return true;
  }

  bool readU64(std::uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 8) {
      std::uint8_t byte = 0;
      if (!readU8(byte)) return false;
      value |= static_cast<std::uint64_t>(byte) << shift;
    }
    return true;
  }

  bool readString(std::size_t size, std::string& value) {
    if (size > remaining()) return false;
    value.assign(reinterpret_cast<const char*>(bytes_.data() + cursor_), size);
    cursor_ += size;
    return true;
  }

  std::size_t remaining() const { return end_ - cursor_; }

 private:
  const std::vector<std::uint8_t>& bytes_;
  std::size_t cursor_ = 0;
  std::size_t end_ = 0;
};

struct Header {
  MessageType type = MessageType::OpenFiles;
  std::uint16_t headerSize = 0;
  std::uint32_t payloadSize = 0;
  std::uint64_t requestId = 0;
};

DecodeError decodeHeader(const std::vector<std::uint8_t>& message,
                         MessageType expectedType, Header& header) {
  if (message.size() < kHeaderSize) return DecodeError::Truncated;
  if (message.size() > kMaxMessageBytes) return DecodeError::InvalidHeader;
  if (!std::equal(kMagic.begin(), kMagic.end(), message.begin())) {
    return DecodeError::InvalidMagic;
  }

  Reader reader(message, kMagic.size(), message.size());
  std::uint16_t major = 0;
  std::uint16_t minor = 0;
  std::uint16_t type = 0;
  std::uint32_t reserved1 = 0;
  std::uint32_t reserved2 = 0;
  if (!reader.readU16(major) || !reader.readU16(minor) ||
      !reader.readU16(type) || !reader.readU16(header.headerSize) ||
      !reader.readU32(header.payloadSize) ||
      !reader.readU64(header.requestId) || !reader.readU32(reserved1) ||
      !reader.readU32(reserved2)) {
    return DecodeError::Truncated;
  }
  (void)minor;
  if (major != kProtocolMajor) return DecodeError::UnsupportedVersion;
  if (type != static_cast<std::uint16_t>(expectedType)) {
    return DecodeError::InvalidMessageType;
  }
  if (header.headerSize < kHeaderSize || header.headerSize > message.size() ||
      header.payloadSize > kMaxMessageBytes || reserved1 != 0 ||
      reserved2 != 0 ||
      static_cast<std::size_t>(header.headerSize) + header.payloadSize !=
          message.size()) {
    return DecodeError::InvalidHeader;
  }
  header.type = expectedType;
  return DecodeError::None;
}

void encodeHeader(Writer& writer, MessageType type, std::uint32_t payloadSize,
                  std::uint64_t requestId) {
  writer.writeMagic();
  writer.writeU16(kProtocolMajor);
  writer.writeU16(kProtocolMinor);
  writer.writeU16(static_cast<std::uint16_t>(type));
  writer.writeU16(kHeaderSize);
  writer.writeU32(payloadSize);
  writer.writeU64(requestId);
  writer.writeU32(0);
  writer.writeU32(0);
}

bool validDirective(std::uint8_t value) {
  return value <= static_cast<std::uint8_t>(
                      OpenPresentationDirective::NativeWindowedFramebuffer);
}

bool validResponseStatus(std::uint32_t value) {
  return value <=
         static_cast<std::uint32_t>(ResponseStatus::InternalError);
}

}  // namespace

std::optional<std::vector<std::uint8_t>> encodeRequest(
    const Request& request) {
  if (request.requestId == 0 || request.openFiles.files.empty() ||
      request.openFiles.files.size() > kMaxFileCount) {
    return std::nullopt;
  }

  Writer payload;
  payload.writeU8(static_cast<std::uint8_t>(request.openFiles.presentation));
  payload.writeU8(0);
  payload.writeU16(0);
  payload.writeU32(
      static_cast<std::uint32_t>(request.openFiles.files.size()));
  for (const std::filesystem::path& file : request.openFiles.files) {
    const std::string path = toUtf8String(file);
    if (path.empty() || path.find('\0') != std::string::npos ||
        !isValidUtf8(path) ||
        path.size() > std::numeric_limits<std::uint32_t>::max()) {
      return std::nullopt;
    }
    payload.writeU32(static_cast<std::uint32_t>(path.size()));
    payload.writeBytes(path);
  }
  std::vector<std::uint8_t> payloadBytes = payload.take();
  if (payloadBytes.size() > kMaxMessageBytes - kHeaderSize) {
    return std::nullopt;
  }

  Writer message;
  encodeHeader(message, MessageType::OpenFiles,
               static_cast<std::uint32_t>(payloadBytes.size()),
               request.requestId);
  std::vector<std::uint8_t> bytes = message.take();
  bytes.insert(bytes.end(), payloadBytes.begin(), payloadBytes.end());
  return bytes;
}

RequestDecodeResult decodeRequest(
    const std::vector<std::uint8_t>& message) {
  Header header;
  const DecodeError headerError =
      decodeHeader(message, MessageType::OpenFiles, header);
  if (headerError != DecodeError::None) return {std::nullopt, headerError};
  if (header.requestId == 0) {
    return {std::nullopt, DecodeError::InvalidPayload};
  }

  Reader payload(message, header.headerSize, message.size());
  std::uint8_t directive = 0;
  std::uint8_t reserved8 = 0;
  std::uint16_t reserved16 = 0;
  std::uint32_t fileCount = 0;
  if (!payload.readU8(directive) || !payload.readU8(reserved8) ||
      !payload.readU16(reserved16) || !payload.readU32(fileCount) ||
      reserved8 != 0 || reserved16 != 0 || !validDirective(directive) ||
      fileCount == 0 || fileCount > kMaxFileCount) {
    return {std::nullopt, DecodeError::InvalidPayload};
  }

  Request request;
  request.requestId = header.requestId;
  request.openFiles.presentation =
      static_cast<OpenPresentationDirective>(directive);
  request.openFiles.files.reserve(fileCount);
  for (std::uint32_t index = 0; index < fileCount; ++index) {
    std::uint32_t length = 0;
    std::string pathBytes;
    if (!payload.readU32(length) || length == 0 ||
        !payload.readString(length, pathBytes)) {
      return {std::nullopt, DecodeError::InvalidPayload};
    }
    if (pathBytes.find('\0') != std::string::npos ||
        !isValidUtf8(pathBytes)) {
      return {std::nullopt, DecodeError::InvalidPayload};
    }
    std::filesystem::path path;
    try {
      path = pathFromUtf8String(pathBytes);
    } catch (...) {
      return {std::nullopt, DecodeError::InvalidPayload};
    }
    if (path.empty()) {
      return {std::nullopt, DecodeError::InvalidPayload};
    }
    request.openFiles.files.push_back(std::move(path));
  }
  if (payload.remaining() != 0) {
    return {std::nullopt, DecodeError::InvalidPayload};
  }
  return {std::move(request), DecodeError::None};
}

std::vector<std::uint8_t> encodeResponse(const Response& response) {
  Writer message;
  encodeHeader(message, MessageType::Response, sizeof(std::uint32_t),
               response.requestId);
  message.writeU32(static_cast<std::uint32_t>(response.status));
  return message.take();
}

ResponseDecodeResult decodeResponse(
    const std::vector<std::uint8_t>& message) {
  Header header;
  const DecodeError headerError =
      decodeHeader(message, MessageType::Response, header);
  if (headerError != DecodeError::None) return {std::nullopt, headerError};

  Reader payload(message, header.headerSize, message.size());
  std::uint32_t status = 0;
  if (!payload.readU32(status) || payload.remaining() != 0 ||
      !validResponseStatus(status)) {
    return {std::nullopt, DecodeError::InvalidPayload};
  }
  return {Response{header.requestId, static_cast<ResponseStatus>(status)},
          DecodeError::None};
}

}  // namespace shell_open_protocol
