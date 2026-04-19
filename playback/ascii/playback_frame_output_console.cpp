#include "playback_frame_output.h"

#include <string>
#include <vector>

namespace playback_frame_output {

void prepareNonAsciiModeFrame(bool allowFrame, int width, int maxHeight,
                             int frameWidth, int frameHeight,
                             FrameOutputState& state,
                             LogLineWriter warningSink) {
  if (!allowFrame) {
    return;
  }
  if (frameWidth <= 0 || frameHeight <= 0) {
    if (warningSink) {
      warningSink("Skipping video frame with invalid dimensions.");
    }
    state.haveFrame = false;
    return;
  }

  state.cachedWidth = width;
  state.cachedMaxHeight = maxHeight;
  state.cachedFrameWidth = frameWidth;
  state.cachedFrameHeight = frameHeight;
}

void renderNonAsciiModeContent(ConsoleScreen& screen, bool windowActive,
                              bool allowFrame, int width, int artTop,
                              int maxHeight, const VideoFrame* frame,
                              int sourceWidth, int sourceHeight,
                              const Style& dimStyle) {
  std::string label = windowActive ? "Video window active (W to toggle back)"
                                   : (allowFrame ? "ASCII rendering disabled"
                                                 : "Waiting for video...");
  screen.writeText(0, artTop, fitLine(label, width), dimStyle);

  if (!allowFrame || !frame || maxHeight <= 1) {
    return;
  }

  int sizeW = frame->width;
  int sizeH = frame->height;
  if (windowActive && sourceWidth > 0 && sourceHeight > 0) {
    sizeW = sourceWidth;
    sizeH = sourceHeight;
  }
  if (sizeW <= 0 || sizeH <= 0) {
    return;
  }

  const std::string sizeLine =
      "Video size: " + std::to_string(sizeW) + "x" + std::to_string(sizeH);
  screen.writeText(0, artTop + 1, fitLine(sizeLine, width), dimStyle);
}

}  // namespace playback_frame_output
