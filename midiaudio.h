#ifndef MIDIAUDIO_H
#define MIDIAUDIO_H

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

class MidiAudioDecoder {
 public:
  MidiAudioDecoder();
  ~MidiAudioDecoder();
  MidiAudioDecoder(MidiAudioDecoder&&) noexcept;
  MidiAudioDecoder& operator=(MidiAudioDecoder&&) noexcept;

  MidiAudioDecoder(const MidiAudioDecoder&) = delete;
  MidiAudioDecoder& operator=(const MidiAudioDecoder&) = delete;

  bool init(const std::filesystem::path& path, uint32_t channels,
            uint32_t sampleRate, std::string* error);
  void uninit();
  bool readFrames(float* out, uint32_t frameCount, uint64_t* framesRead);
  bool seekToFrame(uint64_t frame);
  bool getTotalFrames(uint64_t* outFrames) const;
  bool active() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#endif
