#ifndef VIDEODECODER_H
#define VIDEODECODER_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

struct VideoFrame {
  int width = 0;
  int height = 0;
  int64_t timestamp100ns = 0;
  std::vector<uint8_t> rgba;
};

class VideoDecoder {
 public:
  ~VideoDecoder();
  bool init(const std::filesystem::path& path, std::string* error);
  void uninit();
  bool readFrame(VideoFrame& out);
  bool seekToTimestamp100ns(int64_t timestamp100ns);
  bool atEnd() const;
  int width() const;
  int height() const;
  int64_t duration100ns() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

#endif
