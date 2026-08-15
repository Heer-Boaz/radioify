#ifndef VIDEODECODER_H
#define VIDEODECODER_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>
#define NOMINMAX
#include <d3d11.h>
#include <wrl/client.h>
#include <mutex>

#include "playback/video/color.h"

// Forward declarations
struct ID3D11Device;
struct ID3D11DeviceContext;
struct AVFrame;

// Returns FFmpeg's authoritative packed image size for a decoded software
// frame, or for the software format backing a D3D11 hardware frame.
// Zero means the format cannot be sized and must not enter a bounded cache.
size_t decodedVideoFrameStorageBytes(const AVFrame* frame);

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
  int64_t duration100ns = 0;
  VideoPixelFormat format = VideoPixelFormat::Unknown;
  int rotationQuarterTurns = 0;
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
  std::shared_ptr<AVFrame> hwFrameRef;
  size_t storageBytes = 0;
  // Optional ownership token for independently cached GPU/CPU frame storage.
  // Decoder-backed frames leave this empty; frame-step snapshots use it to
  // release their bounded cache budget when the last copy disappears.
  std::shared_ptr<void> cacheLease;
};

struct VideoReadInfo {
  int64_t timestamp100ns = 0;
  int64_t duration100ns = 0;
  // Stream-time-base identities survive independent decoder instances and let
  // the frame-step cache distinguish repeated presentation timestamps.
  int64_t sourcePtsTicks = (std::numeric_limits<int64_t>::min)();
  int64_t sourceDtsTicks = (std::numeric_limits<int64_t>::min)();
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

struct VideoMetadata {
  int width = 0;
  int height = 0;
  int64_t duration100ns = 0;
  int64_t bitRate = 0;
  bool isHDR = false;
  std::string codecName;
};

// Return non-zero to interrupt a blocking FFmpeg demux operation.  The opaque
// context must outlive the decoder.  This is intentionally a C-style callback:
// FFmpeg invokes it from inside avformat and must not depend on UI/session
// objects or exception-capable callables.
using VideoDecoderInterruptCallback = int (*)(void* opaque);

enum class VideoCpuOutputPrecision : uint8_t {
  EightBit,
  PreserveSource,
};

class VideoDecoder {
 public:
  ~VideoDecoder();
  bool init(const std::filesystem::path& path, std::string* error,
            bool preferHardware = true, bool allowRgbOutput = true,
            VideoStreamSelection* streamSelection = nullptr,
            int requestedStreamIndex = -1,
            VideoDecoderInterruptCallback interruptCallback = nullptr,
            void* interruptOpaque = nullptr,
            VideoCpuOutputPrecision outputPrecision =
                VideoCpuOutputPrecision::EightBit);
  
  // Initialize with an external D3D11 device (for device sharing / zero-copy)
  // key: An optional mutex for synchronizing access to the device context
  bool initWithDevice(const std::filesystem::path& path, 
                      ID3D11Device* device,
                      std::string* error,
                      VideoStreamSelection* streamSelection = nullptr,
                      std::recursive_mutex* contextMutex = nullptr,
                      int requestedStreamIndex = -1);
  
  void uninit();
  bool readFrame(VideoFrame& out, VideoReadInfo* info = nullptr,
                 bool decodePixels = true);
  bool redecodeLastFrame(VideoFrame& out);
  bool setTargetSize(int targetWidth, int targetHeight,
                     std::string* error = nullptr);
  void flush();
  bool seekToTimestamp100ns(int64_t timestamp100ns);
  bool atEnd() const;
  // True only after FFmpeg reported normal demux EOF and the decoder drained.
  // Unlike atEnd(), decode, transfer, and interrupt failures do not satisfy it.
  bool reachedEndOfStream() const;
  int width() const;
  int height() const;
  int64_t duration100ns() const;

 private:
  struct Impl;
  Impl* impl_ = nullptr;
};

bool probeVideoMetadata(const std::filesystem::path& path,
                        VideoMetadata* out,
                        std::string* error);

#endif
