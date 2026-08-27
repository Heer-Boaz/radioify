#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace audio_file {

class FlacWriter {
 public:
  FlacWriter();
  ~FlacWriter();

  FlacWriter(const FlacWriter&) = delete;
  FlacWriter& operator=(const FlacWriter&) = delete;

  bool open(const std::filesystem::path& path, std::uint32_t sampleRate,
            std::uint32_t channels, std::string* error);
  bool writeFrames(const float* interleaved, std::size_t frameCount,
                   std::string* error);
  bool finish(std::string* error);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace audio_file
