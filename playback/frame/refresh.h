#pragma once

#include <cstdint>

#include "player.h"

namespace playback_frame_refresh {

struct PlaybackFrameRefreshState {
  VideoFrame frame;
  bool frameAvailable = false;
  uint64_t frameCounter = 0;
};

struct PlaybackFrameRefreshRequest {
  bool acceptNewFrames = true;
  bool forceRefresh = false;
  bool retainFrameWhenEnded = true;
};

struct PlaybackFrameRefreshResult {
  bool frameAvailable = false;
  bool frameChanged = false;
  bool copiedFrame = false;
  bool sourceHasFrame = false;
};

inline bool isValidFrame(const VideoFrame& frame) {
  return frame.width > 0 && frame.height > 0;
}

inline PlaybackFrameRefreshResult refresh(Player& player,
                                          PlaybackFrameRefreshState& state,
                                          PlaybackFrameRefreshRequest request) {
  PlaybackFrameRefreshResult result{};
  result.sourceHasFrame = player.hasVideoFrame();

  const uint64_t sourceCounter = player.videoFrameCounter();
  const bool sourceChanged = sourceCounter != state.frameCounter;
  const bool shouldCopy =
      result.sourceHasFrame &&
      (request.forceRefresh ||
       (request.acceptNewFrames && (sourceChanged || !state.frameAvailable)));

  if (shouldCopy && player.copyCurrentVideoFrame(&state.frame)) {
    state.frameAvailable = isValidFrame(state.frame);
    state.frameCounter = player.videoFrameCounter();
    result.copiedFrame = true;
  } else if (!result.sourceHasFrame) {
    if (!request.retainFrameWhenEnded || !player.isEnded()) {
      state.frameAvailable = false;
    }
  }

  result.frameAvailable = state.frameAvailable;
  result.frameChanged =
      state.frameAvailable &&
      (result.copiedFrame || request.forceRefresh);
  return result;
}

}  // namespace playback_frame_refresh
