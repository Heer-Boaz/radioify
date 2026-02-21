#ifndef FLACAUDIO_H
#define FLACAUDIO_H

#include <cstdint>
#include <filesystem>
#include <string>

class FlacAudioDecoder {
 public:
  FlacAudioDecoder();
  ~FlacAudioDecoder();

  FlacAudioDecoder(const FlacAudioDecoder&) = delete;
  FlacAudioDecoder& operator=(const FlacAudioDecoder&) = delete;

  bool init(const std::filesystem::path& path, uint32_t channels,
            uint32_t sampleRate, std::string* error);
  void uninit();
  bool readFrames(float* out, uint32_t frameCount, uint64_t* framesRead);
  bool seekToFrame(uint64_t frame);
  bool getTotalFrames(uint64_t* outFrames) const;
  bool active() const { return impl_ != nullptr; }

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

#endif
