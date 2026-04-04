#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "consolescreen.h"
#include "gpu_shared.h"
#include "ui_helpers.h"
#include "videodecoder.h"

namespace playback_frame_output {

// Keep ASCII/video-frame sizing deterministic; maxHeight is already clamped by caller.
using AsciiOutputSizeCalculator =
    std::function<std::pair<int, int>(int width, int maxHeight, int srcW, int srcH)>;
using LogLineWriter = std::function<void(const std::string&)>;

struct AsciiModePrepareInput {
  bool allowFrame = false;
  bool clearHistory = false;
  bool frameChanged = false;
  bool sizeChanged = false;
  bool allowAsciiCpuFallback = false;
  int width = 0;
  int maxHeight = 0;
  AsciiOutputSizeCalculator computeAsciiOutputSize;
  VideoFrame* frame = nullptr;
  AsciiArt* art = nullptr;
  GpuAsciiRenderer* gpuRenderer = nullptr;
  GpuVideoFrameCache* frameCache = nullptr;
  bool* renderFailed = nullptr;
  std::string* renderFailMessage = nullptr;
  std::string* renderFailDetail = nullptr;
  bool* haveFrame = nullptr;
  int* cachedWidth = nullptr;
  int* cachedMaxHeight = nullptr;
  int* cachedFrameWidth = nullptr;
  int* cachedFrameHeight = nullptr;
  LogLineWriter warningSink;
  LogLineWriter timingSink;
};

std::pair<int, int> computeAsciiPlaybackTargetSize(int width, int height,
                                                  int srcW, int srcH,
                                                  bool showStatusLine);

std::pair<int, int> computeAsciiOutputSize(int maxWidth, int maxHeight,
                                          int srcW, int srcH);

bool prepareAsciiModeFrame(AsciiModePrepareInput& input);

void prepareNonAsciiModeFrame(bool allowFrame, int width, int maxHeight,
                             int frameWidth, int frameHeight,
                             int* cachedWidth, int* cachedMaxHeight,
                             int* cachedFrameWidth, int* cachedFrameHeight,
                             LogLineWriter warningSink, bool* haveFrame);

void renderAsciiModeContent(ConsoleScreen& screen, const AsciiArt& art, int width,
                           int height, int maxHeight, int artTop,
                           const std::string& waitingLabel, bool allowFrame,
                           const Style& baseStyle,
                           bool overlayVisible,
                           const std::string& subtitleText,
                           const Style& accentStyle, const Style& dimStyle);

void renderNonAsciiModeContent(ConsoleScreen& screen, bool windowActive,
                              bool allowFrame, int width, int artTop,
                              int maxHeight, const VideoFrame* frame,
                              int sourceWidth, int sourceHeight,
                              const Style& dimStyle);

}  // namespace playback_frame_output
