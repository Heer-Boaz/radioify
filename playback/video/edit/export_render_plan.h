#pragma once

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavutil/rational.h>
}

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "playback/video/edit/decision_list.h"

namespace playback_video_edit::detail {

struct RenderClipWindow {
  int64_t sourceStartUs = 0;
  int64_t sourceEndUs = 0;
};

struct SmoothCutWindow {
  size_t cutIndex = 0;
  uint8_t durationFrames = 0;
  int64_t durationUs = 0;
  int64_t outgoingFrameUs = 0;
  int64_t incomingFrameUs = 0;
};

struct VideoRenderPlan {
  AVRational frameRate{0, 1};
  int64_t frameDurationUs = 0;
  std::vector<RenderClipWindow> clips;
  // Same shape as DecisionList::cutTransitions. Hard cuts have no render
  // window; smooth cuts own one bounded motion-interpolation window.
  std::vector<std::optional<SmoothCutWindow>> cuts;

  bool hasSmoothCuts() const;
};

bool buildVideoRenderPlan(const DecisionList& decisions, AVRational frameRate,
                          VideoRenderPlan* plan, std::string* error);

std::string buildVideoFilterDescription(const VideoRenderPlan& plan,
                                        AVPixelFormat outputFormat,
                                        std::string* error);

}  // namespace playback_video_edit::detail
