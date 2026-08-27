#ifndef FFMPEGAUDIO_H
#define FFMPEGAUDIO_H

#include <cstdint>
#include <filesystem>
#include <string>

struct FfmpegAudioStreamFormat {
  uint32_t channels = 0;
  uint32_t sampleRate = 0;
};

// Reads the format of the best audio stream without decoding it. Exporters use
// this to preserve the source sample rate and channel layout where FLAC can
// represent them directly.
bool probeFfmpegAudioStream(const std::filesystem::path& path,
                            FfmpegAudioStreamFormat* format,
                            std::string* error);

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
  bool getStartOffsetFrames(int64_t* outFrames) const;
  bool getPaddingFrames(uint64_t* outInitial,
                        uint64_t* outTrailing) const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

#endif
