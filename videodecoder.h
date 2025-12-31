#ifndef VIDEODECODER_H
#define VIDEODECODER_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

enum class VideoPixelFormat {
  Unknown,
  RGB32,
  ARGB32,
  NV12,
  P010,
};

struct VideoFrame {
  int width = 0;
  int height = 0;
  int64_t timestamp100ns = 0;
  VideoPixelFormat format = VideoPixelFormat::Unknown;
  int stride = 0;
  int planeHeight = 0;
  bool fullRange = true;
  uint32_t yuvMatrix = 0;
  std::vector<uint8_t> rgba;
  std::vector<uint8_t> yuv;
};

struct VideoReadInfo {
  int64_t timestamp100ns = 0;
  int64_t duration100ns = 0;
  uint32_t flags = 0;
  int streamTicks = 0;
  int typeChanges = 0;
  uint32_t errorHr = 0;
  uint32_t recoveries = 0;
  uint32_t noFrameTimeoutMs = 0;
};

class VideoDecoder {
 public:
  ~VideoDecoder();
  bool init(const std::filesystem::path& path, std::string* error,
            bool preferHardware = true, bool allowRgbOutput = true);
  void uninit();
  bool readFrame(VideoFrame& out, VideoReadInfo* info = nullptr,
                 bool decodePixels = true);
  bool setTargetSize(int targetWidth, int targetHeight,
                     std::string* error = nullptr);
  void flush();
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
