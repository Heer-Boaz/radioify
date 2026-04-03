#ifndef M4ADECODER_H
#define M4ADECODER_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct IMFSourceReader;

class M4aDecoder {
 public:
  bool init(const std::filesystem::path& path, uint32_t channels, uint32_t sampleRate, std::string* error);
  void uninit();
  bool readFrames(float* out, uint32_t frameCount, uint64_t* framesRead);
  bool seekToFrame(uint64_t frame);
  bool getTotalFrames(uint64_t* outFrames) const;
  bool active() const { return reader_ != nullptr; }
  uint32_t channels() const { return channels_; }
  uint32_t sampleRate() const { return sampleRate_; }

 private:
  IMFSourceReader* reader_ = nullptr;
  uint32_t channels_ = 0;
  uint32_t sampleRate_ = 0;
  uint64_t totalFrames_ = 0;
  bool atEnd_ = false;
  std::vector<float> cache_;
  size_t cachePos_ = 0;
};

#endif
