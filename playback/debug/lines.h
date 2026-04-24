#pragma once

#include <cstdio>
#include <string>

#include "playback/video/player.h"

namespace playback_debug_lines {

inline const char* videoFrameFormatLabel(VideoPixelFormat format) {
  switch (format) {
    case VideoPixelFormat::RGB32:
      return "RGB32";
    case VideoPixelFormat::ARGB32:
      return "ARGB32";
    case VideoPixelFormat::NV12:
      return "NV12";
    case VideoPixelFormat::P010:
      return "P010";
    case VideoPixelFormat::HWTexture:
      return "HWTexture";
    case VideoPixelFormat::Unknown:
    default:
      return "unknown";
  }
}

inline const char* yuvMatrixLabel(YuvMatrix matrix) {
  switch (matrix) {
    case YuvMatrix::Bt601:
      return "BT601";
    case YuvMatrix::Bt2020:
      return "BT2020";
    case YuvMatrix::Bt709:
    default:
      return "BT709";
  }
}

inline const char* yuvTransferLabel(YuvTransfer transfer) {
  switch (transfer) {
    case YuvTransfer::Pq:
      return "PQ";
    case YuvTransfer::Hlg:
      return "HLG";
    case YuvTransfer::Sdr:
    default:
      return "SDR";
  }
}

inline std::string videoFrameDebugLine(const PlayerDebugInfo& debug) {
  char line[256];
  std::snprintf(line, sizeof(line),
                "DBG frame ok=%d %dx%d fmt=%s matrix=%s trc=%s full=%d",
                debug.hasVideoFrame ? 1 : 0, debug.videoFrameWidth,
                debug.videoFrameHeight,
                videoFrameFormatLabel(debug.videoFrameFormat),
                yuvMatrixLabel(debug.videoFrameMatrix),
                yuvTransferLabel(debug.videoFrameTransfer),
                debug.videoFrameFullRange ? 1 : 0);
  return std::string(line);
}

}  // namespace playback_debug_lines
