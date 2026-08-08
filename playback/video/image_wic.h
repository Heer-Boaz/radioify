#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "playback/video/image.h"

namespace playback_video_image {

struct DecodeLimits {
  uint32_t maxWidth = 16384;
  uint32_t maxHeight = 16384;
  size_t maxDecodedBytes = 512u * 1024u * 1024u;
};

// Thread-affine Windows Imaging Component codec. open(), all codec calls, and
// close() must execute on the same owning thread.
class WicCodec {
 public:
  WicCodec();
  ~WicCodec();

  WicCodec(const WicCodec&) = delete;
  WicCodec& operator=(const WicCodec&) = delete;

  bool open(std::string* error = nullptr);
  void close();
  bool isOpen() const;

  bool decodeFile(const std::filesystem::path& path, RgbaImage* image,
                  const DecodeLimits& limits = {},
                  std::string* error = nullptr);
  bool decodeBytes(const uint8_t* bytes, size_t size, RgbaImage* image,
                   const DecodeLimits& limits = {},
                   std::string* error = nullptr);
  bool encodePng(const RgbaImageView& image, std::vector<uint8_t>* png,
                 std::string* error = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_video_image
