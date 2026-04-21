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
    std::function<std::pair<int, int>(int width, int maxHeight, int srcW,
                                      int srcH, double cellPixelWidth,
                                      double cellPixelHeight)>;
using LogLineWriter = std::function<void(const std::string&)>;

struct FrameOutputState {
  bool renderFailed = false;
  std::string renderFailMessage;
  std::string renderFailDetail;
  std::string lastRenderPath;
  bool haveFrame = false;
  int cachedWidth = -1;
  int cachedMaxHeight = -1;
  int cachedFrameWidth = -1;
  int cachedFrameHeight = -1;
  int cachedLayoutSourceWidth = -1;
  int cachedLayoutSourceHeight = -1;
  double cachedCellPixelWidth = -1.0;
  double cachedCellPixelHeight = -1.0;
  int progressBarX = -1;
  int progressBarY = -1;
  int progressBarWidth = 0;
};

struct AsciiModePrepareInput {
  bool allowFrame = false;
  bool clearHistory = false;
  bool frameChanged = false;
  bool sizeChanged = false;
  bool allowAsciiCpuFallback = false;
  int width = 0;
  int maxHeight = 0;
  double cellPixelWidth = 0.0;
  double cellPixelHeight = 0.0;
  int sourceWidth = 0;
  int sourceHeight = 0;
  AsciiOutputSizeCalculator computeAsciiOutputSize;
  VideoFrame* frame = nullptr;
  AsciiArt* art = nullptr;
  GpuAsciiRenderer* gpuRenderer = nullptr;
  GpuVideoFrameCache* frameCache = nullptr;
  FrameOutputState* state = nullptr;
  LogLineWriter warningSink;
  LogLineWriter timingSink;
};

std::pair<int, int> computeAsciiPlaybackTargetSize(int width, int height,
                                                  int srcW, int srcH,
                                                  double cellPixelWidth,
                                                  double cellPixelHeight,
                                                  bool showStatusLine);

std::pair<int, int> computeAsciiOutputSize(int maxWidth, int maxHeight,
                                          int srcW, int srcH,
                                          double cellPixelWidth,
                                          double cellPixelHeight);

inline int centerContentTop(int top, int containerHeight, int contentHeight) {
  if (contentHeight <= 0 || containerHeight <= contentHeight) {
    return top;
  }
  return top + (containerHeight - contentHeight) / 2;
}

bool prepareAsciiModeFrame(AsciiModePrepareInput& input);

void prepareNonAsciiModeFrame(bool allowFrame, int width, int maxHeight,
                             int frameWidth, int frameHeight,
                             FrameOutputState& state,
                             LogLineWriter warningSink);

void renderAsciiModeContent(ConsoleScreen& screen, const AsciiArt& art, int width,
                           int height, int maxHeight, int artTop,
                           const std::string& waitingLabel, bool allowFrame,
                           const Style& baseStyle,
                           bool overlayVisible,
                           int overlayReservedLines,
                           const Style& dimStyle);

void renderNonAsciiModeContent(ConsoleScreen& screen, bool windowActive,
                              bool allowFrame, int width, int artTop,
                              int maxHeight, const VideoFrame* frame,
                              int sourceWidth, int sourceHeight,
                              const Style& dimStyle);

}  // namespace playback_frame_output
