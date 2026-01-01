#ifndef FFMPEGAUDIO_H
#define FFMPEGAUDIO_H

#include <cstdint>
#include <filesystem>
#include <string>

class FfmpegAudioDecoder {
 public:
  FfmpegAudioDecoder();
  ~FfmpegAudioDecoder();

  bool init(const std::filesystem::path& path, uint32_t channels,
            uint32_t sampleRate, std::string* error);
  void uninit();
  bool readFrames(float* out, uint32_t frameCount, uint64_t* framesRead);
  bool seekToFrame(uint64_t frame);
  bool getTotalFrames(uint64_t* outFrames) const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

#endif
