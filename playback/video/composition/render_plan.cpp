#include "playback/video/composition/render_plan.h"

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <utility>

namespace playback_video_composition {
namespace {

constexpr AVRational kMicrosecondTimeBase{1, 1'000'000};

void setError(std::string* destination, const std::string& message) {
  if (destination) *destination = message;
}

bool addWithoutOverflow(int64_t left, int64_t right, int64_t* result) {
  if (!result || right < 0 ||
      left > (std::numeric_limits<int64_t>::max)() - right) {
    return false;
  }
  *result = left + right;
  return true;
}

int64_t frameOffsetUs(int64_t frames, AVRational frameRate) {
  return av_rescale_q_rnd(
      frames, av_inv_q(frameRate), kMicrosecondTimeBase,
      static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
}

int64_t timestampToleranceUs(AVRational frameRate,
                             AVRational sourceTimeBase,
                             int64_t frameDurationUs) {
  if (frameRate.num <= 0 || frameRate.den <= 0 ||
      sourceTimeBase.num <= 0 || sourceTimeBase.den <= 0 ||
      frameDurationUs <= 0) {
    return 0;
  }
  const int64_t sourceTickUs = std::llabs(av_rescale_q_rnd(
      1, sourceTimeBase, kMicrosecondTimeBase,
      static_cast<AVRounding>(AV_ROUND_UP | AV_ROUND_PASS_MINMAX)));
  const int64_t ticksPerFrameNumerator =
      static_cast<int64_t>(frameRate.den) * sourceTimeBase.den;
  const int64_t ticksPerFrameDenominator =
      static_cast<int64_t>(frameRate.num) * sourceTimeBase.num;
  const bool exactFrameClock = ticksPerFrameDenominator > 0 &&
                               ticksPerFrameNumerator > 0 &&
                               ticksPerFrameNumerator %
                                       ticksPerFrameDenominator ==
                                   0;
  // With an integral number of source ticks per frame, only the conversion to
  // microseconds can differ. Otherwise two independently quantized frame PTS
  // values can differ from the ideal cadence by one complete source tick.
  const int64_t toleranceUs = exactFrameClock ? 2 : sourceTickUs + 1;
  return toleranceUs <= (frameDurationUs - 1) / 2 ? toleranceUs : 0;
}

int64_t trimBoundaryUs(const RenderPlan& plan, int64_t boundaryUs) {
  return (std::max)(int64_t{0}, boundaryUs - plan.timestampToleranceUs);
}

bool validateTimingSequence(const std::vector<int64_t>& expected,
                            const std::vector<SourceFrameTiming>& actual,
                            int64_t toleranceUs) {
  if (expected.size() != actual.size()) return false;
  for (size_t index = 0; index < expected.size(); ++index) {
    if (!sourceTimestampMatches(actual[index].ptsUs, expected[index],
                                toleranceUs)) {
      return false;
    }
    if (index + 1 < expected.size()) {
      if (actual[index + 1].ptsUs <= actual[index].ptsUs) return false;
      if (actual[index].durationUs > 0) {
        int64_t endUs = 0;
        if (!addWithoutOverflow(actual[index].ptsUs,
                                actual[index].durationUs, &endUs) ||
            !sourceTimestampMatches(endUs, expected[index + 1],
                                    toleranceUs * 2)) {
          return false;
        }
      }
    }
  }
  return true;
}

bool validTransitionShape(
    size_t rangeCount,
    const std::vector<playback_video_sequence::Transition>& transitions) {
  if (rangeCount == 0 || transitions.size() != rangeCount - 1) return false;
  return std::all_of(
      transitions.begin(), transitions.end(), [](const auto& transition) {
        switch (transition.kind) {
          case playback_video_sequence::TransitionKind::Hard:
            return transition.outgoingFrames == 0 &&
                   transition.incomingFrames == 0;
          case playback_video_sequence::TransitionKind::MotionSmooth:
            return transition.outgoingFrames > 0 &&
                   transition.incomingFrames > 0 &&
                   transition.durationFrames() >=
                       playback_video_sequence::kMinimumTransitionFrames &&
                   transition.durationFrames() <=
                       playback_video_sequence::kMaximumTransitionFrames;
        }
        return false;
      });
}

bool validSourceRanges(
    const std::vector<playback_video_sequence::SourceRange>& ranges) {
  int64_t previousEndUs = -1;
  for (const auto& range : ranges) {
    if (range.startUs < 0 || range.endUs <= range.startUs ||
        range.startUs < previousEndUs) {
      return false;
    }
    previousEndUs = range.endUs;
  }
  return !ranges.empty();
}

std::string frameRateText(AVRational rate) {
  return std::to_string(rate.num) + "/" + std::to_string(rate.den);
}

AVPixelFormat motionWorkingFormat(AVPixelFormat outputFormat) {
  switch (outputFormat) {
    case AV_PIX_FMT_NV12:
      return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_P010LE:
      return AV_PIX_FMT_YUV420P10LE;
    default:
      return outputFormat;
  }
}

bool appendPreparedMotionInterpolation(
    std::string* description, const RenderPlan& plan,
    const MotionTransitionWindow& window, const std::string& input,
    const std::string& output, AVPixelFormat outputFormat,
    std::string* error) {
  if (!description || plan.frameDurationUs <= 0 ||
      window.durationFrames <
          playback_video_sequence::kMinimumTransitionFrames) {
    setError(error, "The motion transition render window is invalid.");
    return false;
  }
  const std::string frameTimeBase =
      std::to_string(plan.frameRate.den) + "/" +
      std::to_string(plan.frameRate.num);
  const char* workingFormat =
      av_get_pix_fmt_name(motionWorkingFormat(outputFormat));
  if (!workingFormat) {
    setError(error, "The motion-interpolation working format is not named.");
    return false;
  }
  *description += input + "format=pix_fmts=" + workingFormat +
                  ",settb=" + frameTimeBase + ",setpts=N+gte(N\\,2)*" +
                  std::to_string(window.durationFrames - 1) +
                  ",minterpolate=fps=" + frameRateText(plan.frameRate) +
                  ":mi_mode=mci:mc_mode=aobmc:me_mode=bidir:vsbmc=1:"
                  "scd=none,trim=start_frame=1:end_frame=" +
                  std::to_string(window.durationFrames + 1) +
                  ",setpts=PTS-STARTPTS" + output + ";";
  return true;
}

bool appendMotionTransition(
    std::string* description, const RenderPlan& plan,
    const MotionTransitionWindow& window, const std::string& leftInput,
    const std::string& rightInput, const std::string& output,
    AVPixelFormat outputFormat, std::string* error) {
  if (!description) return false;
  const std::string number = std::to_string(window.cutIndex);
  const std::string left = "motion_l" + number;
  const std::string right = "motion_r" + number;
  const std::string prepared = "motion_input" + number;

  *description += leftInput + "trim=start_pts=" +
                  std::to_string(trimBoundaryUs(
                      plan, window.outgoingContextStartUs)) +
                  ":end_pts=" +
                  std::to_string(trimBoundaryUs(
                      plan, window.outgoingContextEndUs)) +
                  ",setpts=PTS-STARTPTS[" + left + "];";
  *description += rightInput + "trim=start_pts=" +
                  std::to_string(trimBoundaryUs(
                      plan, window.incomingAnchorUs)) +
                  ":end_pts=" +
                  std::to_string(trimBoundaryUs(
                      plan, window.incomingContextEndUs)) +
                  ",setpts=PTS-STARTPTS[" + right + "];";
  *description += "[" + left + "][" + right +
                  "]concat=n=2:v=1:a=0[" + prepared + "];";
  return appendPreparedMotionInterpolation(
      description, plan, window, "[" + prepared + "]", output,
      outputFormat, error);
}

const char* pixelFormatName(AVPixelFormat format, std::string* error) {
  const char* name = av_get_pix_fmt_name(format);
  if (!name) setError(error, "The preserving video pixel format is not named.");
  return name;
}

}  // namespace

bool RenderPlan::hasMotionTransitions() const {
  return !motionTransitions.empty();
}

bool sourceTimestampMatches(int64_t actualUs, int64_t expectedUs,
                            int64_t toleranceUs) {
  if (actualUs < 0 || expectedUs < 0 || toleranceUs < 0) return false;
  return actualUs >= expectedUs ? actualUs - expectedUs <= toleranceUs
                                : expectedUs - actualUs <= toleranceUs;
}

std::optional<size_t> RenderPlan::motionTransitionIndexNear(
    int64_t presentationUs, int64_t maximumDistanceUs) const {
  if (presentationUs < 0 || maximumDistanceUs < 0 ||
      motionTransitions.empty()) {
    return std::nullopt;
  }
  const auto next = std::lower_bound(
      motionTransitions.begin(), motionTransitions.end(), presentationUs,
      [](const MotionTransitionWindow& window, int64_t positionUs) {
        return window.presentationStartUs < positionUs;
      });

  std::optional<size_t> selected;
  int64_t selectedDistance = (std::numeric_limits<int64_t>::max)();
  const auto consider = [&](auto candidate) {
    if (candidate == motionTransitions.end() ||
        candidate->presentationStartUs < 0 || candidate->durationUs <= 0 ||
        candidate->presentationStartUs >
            (std::numeric_limits<int64_t>::max)() - candidate->durationUs) {
      return;
    }
    const int64_t endUs =
        candidate->presentationStartUs + candidate->durationUs;
    int64_t distanceUs = 0;
    if (presentationUs < candidate->presentationStartUs) {
      distanceUs = candidate->presentationStartUs - presentationUs;
    } else if (presentationUs >= endUs) {
      distanceUs = presentationUs - endUs;
    }
    if (distanceUs <= maximumDistanceUs &&
        distanceUs < selectedDistance) {
      selected = static_cast<size_t>(candidate - motionTransitions.begin());
      selectedDistance = distanceUs;
    }
  };
  if (next != motionTransitions.begin()) consider(std::prev(next));
  consider(next);
  return selected;
}

bool buildRenderPlan(
    const std::vector<playback_video_sequence::SourceRange>& ranges,
    const std::vector<playback_video_sequence::Transition>& transitions,
    SourceTiming sourceTiming, RenderPlan* plan, std::string* error) {
  if (!plan) {
    setError(error, "The composition render destination is absent.");
    return false;
  }
  if (!validSourceRanges(ranges)) {
    setError(error, "The composition source ranges are invalid.");
    return false;
  }
  if (!validTransitionShape(ranges.size(), transitions)) {
    setError(error, "The composition transition list is invalid.");
    return false;
  }

  RenderPlan next;
  next.frameRate = sourceTiming.frameRate;
  next.clips.reserve(ranges.size());
  std::vector<std::optional<MotionTransitionWindow>> cutWindows(
      transitions.size());
  const bool hasMotion = std::any_of(
      transitions.begin(), transitions.end(), [](const auto& transition) {
        return transition.kind ==
               playback_video_sequence::TransitionKind::MotionSmooth;
      });
  if (hasMotion &&
      (sourceTiming.frameRate.num <= 0 || sourceTiming.frameRate.den <= 0 ||
       sourceTiming.timeBase.num <= 0 || sourceTiming.timeBase.den <= 0 ||
       av_q2d(sourceTiming.frameRate) < 1.0 ||
       av_q2d(sourceTiming.frameRate) > 240.0)) {
    setError(error,
             "Smooth cut requires a valid source timebase and a stable frame "
             "rate between 1 and 240 fps.");
    return false;
  }
  if (hasMotion) {
    next.motionTransitions.reserve(static_cast<size_t>(std::count_if(
        transitions.begin(), transitions.end(), [](const auto& transition) {
          return transition.kind ==
                 playback_video_sequence::TransitionKind::MotionSmooth;
        })));
    next.frameDurationUs = av_rescale_q_rnd(
        1, av_inv_q(sourceTiming.frameRate), kMicrosecondTimeBase,
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    if (next.frameDurationUs <= 0) {
      setError(error, "Smooth cut could not resolve the sequence timebase.");
      return false;
    }
    next.timestampToleranceUs = timestampToleranceUs(
        sourceTiming.frameRate, sourceTiming.timeBase,
        next.frameDurationUs);
    if (next.timestampToleranceUs <= 0) {
      setError(error,
               "Smooth cut cannot identify individual frames in this source "
               "timebase; use a hard cut.");
      return false;
    }
  }

  int64_t presentationCutUs = 0;
  for (size_t cutIndex = 0; cutIndex < transitions.size(); ++cutIndex) {
    if (!addWithoutOverflow(presentationCutUs,
                            ranges[cutIndex].durationUs(),
                            &presentationCutUs)) {
      setError(error, "The composition duration overflowed.");
      return false;
    }
    const auto& transition = transitions[cutIndex];
    if (transition.kind !=
        playback_video_sequence::TransitionKind::MotionSmooth) {
      continue;
    }
    const uint8_t durationFrames =
        static_cast<uint8_t>(transition.durationFrames());
    const int64_t outgoingUs =
        frameOffsetUs(transition.outgoingFrames, sourceTiming.frameRate);
    const int64_t durationUs =
        frameOffsetUs(durationFrames, sourceTiming.frameRate);
    const int64_t outgoingContextOffsetUs = frameOffsetUs(
        static_cast<int64_t>(transition.outgoingFrames) + 1,
        sourceTiming.frameRate);
    const int64_t outgoingContextEndOffsetUs = frameOffsetUs(
        static_cast<int64_t>(transition.outgoingFrames) - 1,
        sourceTiming.frameRate);
    const int64_t incomingAnchorOffsetUs =
        frameOffsetUs(transition.incomingFrames, sourceTiming.frameRate);
    const int64_t incomingContextEndOffsetUs = frameOffsetUs(
        static_cast<int64_t>(transition.incomingFrames) + 2,
        sourceTiming.frameRate);
    const auto& outgoingRange = ranges[cutIndex];
    const auto& incomingRange = ranges[cutIndex + 1];
    if (outgoingUs <= 0 || durationUs <= 0 ||
        outgoingContextOffsetUs <= 0 ||
        outgoingContextOffsetUs > outgoingRange.endUs ||
        incomingAnchorOffsetUs <= 0 || incomingContextEndOffsetUs <= 0 ||
        incomingRange.startUs >
            (std::numeric_limits<int64_t>::max)() -
                incomingContextEndOffsetUs) {
      setError(error, "A smooth-cut source window overflowed.");
      return false;
    }
    const int64_t outgoingContextStartUs =
        outgoingRange.endUs - outgoingContextOffsetUs;
    const int64_t outgoingAnchorUs = outgoingRange.endUs - outgoingUs;
    const int64_t outgoingContextEndUs =
        outgoingRange.endUs - outgoingContextEndOffsetUs;
    const int64_t incomingAnchorUs =
        incomingRange.startUs + incomingAnchorOffsetUs;
    const int64_t incomingContextEndUs =
        incomingRange.startUs + incomingContextEndOffsetUs;
    if (outgoingContextStartUs < outgoingRange.startUs ||
        outgoingAnchorUs <= outgoingContextStartUs ||
        outgoingContextEndUs <= outgoingAnchorUs ||
        incomingAnchorUs <= incomingRange.startUs ||
        incomingContextEndUs <= incomingAnchorUs ||
        incomingContextEndUs > incomingRange.endUs) {
      setError(error,
               "A clip next to smooth cut " +
                   std::to_string(cutIndex + 1) +
                   " is too short for its " +
                   std::to_string(durationFrames) +
                   "-frame transition.");
      return false;
    }
    MotionTransitionWindow window;
    window.cutIndex = cutIndex;
    window.outgoingFrames = transition.outgoingFrames;
    window.incomingFrames = transition.incomingFrames;
    window.durationFrames = durationFrames;
    window.durationUs = durationUs;
    window.presentationStartUs = presentationCutUs - outgoingUs;
    window.outgoingContextStartUs = outgoingContextStartUs;
    window.outgoingAnchorUs = outgoingAnchorUs;
    window.outgoingContextEndUs = outgoingContextEndUs;
    window.incomingAnchorUs = incomingAnchorUs;
    window.incomingContextEndUs = incomingContextEndUs;
    cutWindows[cutIndex] = window;
    next.motionTransitions.push_back(window);
  }

  for (size_t clipIndex = 0; clipIndex < ranges.size(); ++clipIndex) {
    int64_t startUs = ranges[clipIndex].startUs;
    int64_t endUs = ranges[clipIndex].endUs;
    if (clipIndex > 0 && cutWindows[clipIndex - 1]) {
      startUs = cutWindows[clipIndex - 1]->incomingAnchorUs;
    }
    if (clipIndex < cutWindows.size() && cutWindows[clipIndex]) {
      endUs = cutWindows[clipIndex]->outgoingAnchorUs;
    }
    if (endUs <= startUs) {
      setError(error,
               "Smooth-cut overlaps meet inside clip " +
                   std::to_string(clipIndex + 1) + ".");
      return false;
    }
    next.clips.push_back({startUs, endUs});
  }

  *plan = std::move(next);
  return true;
}

bool motionSourceTiming(const RenderPlan& plan,
                        const MotionTransitionWindow& transition,
                        MotionSourceTiming* timing, std::string* error) {
  if (!timing || plan.frameRate.num <= 0 || plan.frameRate.den <= 0 ||
      transition.outgoingFrames == 0 || transition.incomingFrames == 0) {
    setError(error, "The motion transition source timing is invalid.");
    return false;
  }
  const int64_t outgoingRangeEndUs =
      transition.outgoingAnchorUs +
      frameOffsetUs(transition.outgoingFrames, plan.frameRate);
  const int64_t incomingRangeStartUs =
      transition.incomingAnchorUs -
      frameOffsetUs(transition.incomingFrames, plan.frameRate);
  if (outgoingRangeEndUs <= transition.outgoingAnchorUs ||
      incomingRangeStartUs < 0) {
    setError(error, "The motion transition source timing overflowed.");
    return false;
  }

  MotionSourceTiming next;
  next.outgoingPtsUs.reserve(
      static_cast<size_t>(transition.outgoingFrames) + 2);
  for (int64_t index = 0;
       index <= static_cast<int64_t>(transition.outgoingFrames) + 1;
       ++index) {
    const int64_t framesBeforeEnd =
        static_cast<int64_t>(transition.outgoingFrames) + 1 - index;
    next.outgoingPtsUs.push_back(
        outgoingRangeEndUs - frameOffsetUs(framesBeforeEnd, plan.frameRate));
  }
  next.incomingPtsUs.reserve(
      static_cast<size_t>(transition.incomingFrames) + 2);
  for (int64_t index = 0;
       index <= static_cast<int64_t>(transition.incomingFrames) + 1;
       ++index) {
    next.incomingPtsUs.push_back(
        incomingRangeStartUs + frameOffsetUs(index, plan.frameRate));
  }
  *timing = std::move(next);
  return true;
}

bool validateMotionSourceTiming(
    const RenderPlan& plan, const MotionTransitionWindow& transition,
    const std::vector<SourceFrameTiming>& outgoing,
    const std::vector<SourceFrameTiming>& incoming, std::string* error) {
  MotionSourceTiming expected;
  if (!motionSourceTiming(plan, transition, &expected, error)) return false;
  if (plan.timestampToleranceUs <= 0 ||
      !validateTimingSequence(expected.outgoingPtsUs, outgoing,
                              plan.timestampToleranceUs) ||
      !validateTimingSequence(expected.incomingPtsUs, incoming,
                              plan.timestampToleranceUs)) {
    setError(error,
             "Smooth cut " + std::to_string(transition.cutIndex + 1) +
                 " is not aligned to a stable source-frame cadence; use a "
                 "hard cut for this edit point.");
    return false;
  }
  return true;
}

std::string buildProgramFilterDescription(const RenderPlan& plan,
                                           AVPixelFormat outputFormat,
                                           std::string* error) {
  size_t previousCutIndex = 0;
  bool firstTransition = true;
  const bool validTransitions = std::all_of(
      plan.motionTransitions.begin(), plan.motionTransitions.end(),
      [&](const MotionTransitionWindow& transition) {
        const bool valid = transition.cutIndex < plan.clips.size() &&
                           transition.cutIndex + 1 < plan.clips.size() &&
                           (firstTransition ||
                            transition.cutIndex > previousCutIndex);
        previousCutIndex = transition.cutIndex;
        firstTransition = false;
        return valid;
      });
  if (plan.clips.empty() || !validTransitions) {
    setError(error, "The video composition render plan has an invalid shape.");
    return {};
  }
  const char* formatName = pixelFormatName(outputFormat, error);
  if (!formatName) return {};

  const size_t branchCount =
      plan.clips.size() + plan.motionTransitions.size() * 2;
  std::string description = "[src]split=" + std::to_string(branchCount);
  for (size_t clip = 0; clip < plan.clips.size(); ++clip) {
    description += "[raw_m" + std::to_string(clip) + "]";
  }
  for (const auto& transition : plan.motionTransitions) {
    description += "[raw_l" + std::to_string(transition.cutIndex) + "]";
    description += "[raw_r" + std::to_string(transition.cutIndex) + "]";
  }
  description += ";";

  for (size_t clip = 0; clip < plan.clips.size(); ++clip) {
    const ClipWindow& window = plan.clips[clip];
    description += "[raw_m" + std::to_string(clip) +
                   "]trim=start_pts=" +
                   std::to_string(trimBoundaryUs(plan,
                                                 window.sourceStartUs)) +
                   ":end_pts=" +
                   std::to_string(trimBoundaryUs(plan,
                                                 window.sourceEndUs));
    description += ",setpts=PTS-STARTPTS[m" + std::to_string(clip) + "];";
  }
  for (const auto& transition : plan.motionTransitions) {
    const size_t cut = transition.cutIndex;
    if (!appendMotionTransition(
            &description, plan, transition,
            "[raw_l" + std::to_string(cut) + "]",
            "[raw_r" + std::to_string(cut) + "]",
            "[s" + std::to_string(cut) + "]", outputFormat, error)) {
      return {};
    }
  }

  const size_t partCount =
      plan.clips.size() + plan.motionTransitions.size();
  size_t transitionIndex = 0;
  for (size_t clip = 0; clip < plan.clips.size(); ++clip) {
    description += "[m" + std::to_string(clip) + "]";
    if (transitionIndex < plan.motionTransitions.size() &&
        plan.motionTransitions[transitionIndex].cutIndex == clip) {
      description += "[s" + std::to_string(clip) + "]";
      ++transitionIndex;
    }
  }
  description += "concat=n=" + std::to_string(partCount) +
                 ":v=1:a=0[cat];[cat]format=pix_fmts=" + formatName +
                 "[out]";
  return description;
}

std::string buildTransitionFilterDescription(
    const RenderPlan& plan, const MotionTransitionWindow& transition,
    AVPixelFormat outputFormat, std::string* error) {
  const char* formatName = pixelFormatName(outputFormat, error);
  if (!formatName) return {};
  std::string description;
  if (!appendPreparedMotionInterpolation(&description, plan, transition,
                                         "[src]", "[motion]", outputFormat,
                                         error)) {
    return {};
  }
  description += "[motion]format=pix_fmts=" + std::string(formatName) +
                 "[out]";
  return description;
}

}  // namespace playback_video_composition
