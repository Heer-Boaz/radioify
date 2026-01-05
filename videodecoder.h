#ifndef VIDEODECODER_H
#define VIDEODECODER_H

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#define NOMINMAX
#include <d3d11.h>
#include <wrl/client.h>

#include "videocolor.h"

// Forward declarations
struct ID3D11Device;
struct ID3D11DeviceContext;

enum class VideoPixelFormat {
  Unknown,
  RGB32,
  ARGB32,
  NV12,
  P010,
  HWTexture,  // Direct GPU texture (zero-copy path)
};

struct VideoFrame {
  int width = 0;
  int height = 0;
  int64_t timestamp100ns = 0;
  VideoPixelFormat format = VideoPixelFormat::Unknown;
  int stride = 0;
  int planeHeight = 0;
  bool fullRange = true;
  YuvMatrix yuvMatrix = YuvMatrix::Bt709;
  YuvTransfer yuvTransfer = YuvTransfer::Sdr;
  std::vector<uint8_t> rgba;
  std::vector<uint8_t> yuv;
  
  // Hardware texture for zero-copy GPU path
  // Only valid when format == HWTexture
  Microsoft::WRL::ComPtr<ID3D11Texture2D> hwTexture;
  int hwTextureArrayIndex = 0;  // Index into texture array (for D3D11VA pools)
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

struct VideoStreamInfo {
  int index = -1;
  int width = 0;
  int height = 0;
  int64_t bitRate = 0;
  bool isDefault = false;
  bool isAttachedPic = false;
  bool hasDecoder = false;
  std::string codecName;
};

struct VideoStreamSelection {
  std::vector<VideoStreamInfo> streams;
  int selectedIndex = -1;
};

class VideoDecoder {
 public:
  ~VideoDecoder();
  bool init(const std::filesystem::path& path, std::string* error,
            bool preferHardware = true, bool allowRgbOutput = true,
            VideoStreamSelection* streamSelection = nullptr);
  
  // Initialize with an external D3D11 device (for device sharing / zero-copy)
  bool initWithDevice(const std::filesystem::path& path, 
                      ID3D11Device* device,
                      std::string* error,
                      VideoStreamSelection* streamSelection = nullptr);
  
  void uninit();
  bool readFrame(VideoFrame& out, VideoReadInfo* info = nullptr,
                 bool decodePixels = true);
  bool redecodeLastFrame(VideoFrame& out);
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
