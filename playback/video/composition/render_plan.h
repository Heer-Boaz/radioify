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

#include "playback/video/sequence.h"

namespace playback_video_composition {

struct SourceTiming {
  AVRational frameRate{0, 1};
  AVRational timeBase{0, 1};
};

struct ClipWindow {
  int64_t sourceStartUs = 0;
  int64_t sourceEndUs = 0;
};

struct MotionTransitionWindow {
  size_t cutIndex = 0;
  uint8_t outgoingFrames = 0;
  uint8_t incomingFrames = 0;
  uint8_t durationFrames = 0;
  int64_t durationUs = 0;
  int64_t presentationStartUs = 0;
  int64_t outgoingContextStartUs = 0;
  int64_t outgoingAnchorUs = 0;
  int64_t outgoingContextEndUs = 0;
  int64_t incomingAnchorUs = 0;
  int64_t incomingContextEndUs = 0;
};

struct SourceFrameTiming {
  int64_t ptsUs = 0;
  int64_t durationUs = 0;
};

// Bounded consecutive source neighborhoods used to prove that a motion
// transition is frame-exact before either preview or export renders it.
struct MotionSourceTiming {
  std::vector<int64_t> outgoingPtsUs;
  std::vector<int64_t> incomingPtsUs;
};

// Immutable render projection shared by interactive preview and final export.
// Transport continues to use the source clip concatenation; this plan owns
// only the overlapping visual contribution at each transition.
struct RenderPlan {
  AVRational frameRate{0, 1};
  int64_t frameDurationUs = 0;
  int64_t timestampToleranceUs = 0;
  std::vector<ClipWindow> clips;
  // Only executable transition nodes belong in the render plan. Hard cuts
  // remain a timeline decision and require no render node.
  std::vector<MotionTransitionWindow> motionTransitions;

  bool hasMotionTransitions() const;
  std::optional<size_t> motionTransitionIndexNear(
      int64_t presentationUs, int64_t maximumDistanceUs) const;
};

bool buildRenderPlan(
    const std::vector<playback_video_sequence::SourceRange>& ranges,
    const std::vector<playback_video_sequence::Transition>& transitions,
    SourceTiming sourceTiming, RenderPlan* plan, std::string* error);

bool motionSourceTiming(const RenderPlan& plan,
                        const MotionTransitionWindow& transition,
                        MotionSourceTiming* timing, std::string* error);

bool sourceTimestampMatches(int64_t actualUs, int64_t expectedUs,
                            int64_t toleranceUs);

bool validateMotionSourceTiming(
    const RenderPlan& plan, const MotionTransitionWindow& transition,
    const std::vector<SourceFrameTiming>& outgoing,
    const std::vector<SourceFrameTiming>& incoming, std::string* error);

// Full edited-program graph used by export.
std::string buildProgramFilterDescription(const RenderPlan& plan,
                                          AVPixelFormat outputFormat,
                                          std::string* error);

// One transition graph used by the bounded interactive render cache. It
// consumes exactly four ordered context frames on [src] (two outgoing, then
// two incoming) and publishes [out].
std::string buildTransitionFilterDescription(
    const RenderPlan& plan, const MotionTransitionWindow& transition,
    AVPixelFormat outputFormat, std::string* error);

}  // namespace playback_video_composition
