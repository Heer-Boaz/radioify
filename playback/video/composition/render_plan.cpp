#include "playback/video/composition/render_plan.h"

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
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

int64_t frameIndexAtUs(int64_t sourceUs, AVRational frameRate) {
  return av_rescale_q_rnd(
      sourceUs, kMicrosecondTimeBase, av_inv_q(frameRate),
      static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
}

int64_t frameOffsetUs(int64_t frames, AVRational frameRate) {
  return av_rescale_q_rnd(
      frames, av_inv_q(frameRate), kMicrosecondTimeBase,
      static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
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

  *description += leftInput + "trim=start_frame=" +
                  std::to_string(window.outgoingContextStartFrame) +
                  ":end_frame=" +
                  std::to_string(window.outgoingAnchorFrame + 1) +
                  ",setpts=PTS-STARTPTS[" + left + "];";
  *description += rightInput + "trim=start_frame=" +
                  std::to_string(window.incomingAnchorFrame) +
                  ":end_frame=" +
                  std::to_string(window.incomingContextEndFrame) +
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
  return std::any_of(cuts.begin(), cuts.end(),
                     [](const auto& cut) { return cut.has_value(); });
}

bool buildRenderPlan(
    const std::vector<playback_video_sequence::SourceRange>& ranges,
    const std::vector<playback_video_sequence::Transition>& transitions,
    AVRational frameRate, RenderPlan* plan, std::string* error) {
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
  next.frameRate = frameRate;
  next.clips.reserve(ranges.size());
  next.cuts.resize(transitions.size());
  const bool hasMotion = std::any_of(
      transitions.begin(), transitions.end(), [](const auto& transition) {
        return transition.kind ==
               playback_video_sequence::TransitionKind::MotionSmooth;
      });
  if (hasMotion &&
      (frameRate.num <= 0 || frameRate.den <= 0 ||
       av_q2d(frameRate) < 1.0 || av_q2d(frameRate) > 240.0)) {
    setError(error,
             "Smooth cut requires a stable sequence frame rate between 1 "
             "and 240 fps.");
    return false;
  }
  if (hasMotion) {
    next.frameDurationUs = av_rescale_q_rnd(
        1, av_inv_q(frameRate), kMicrosecondTimeBase,
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    if (next.frameDurationUs <= 0) {
      setError(error, "Smooth cut could not resolve the sequence timebase.");
      return false;
    }
  }

  std::vector<int64_t> rangeStartFrames;
  std::vector<int64_t> rangeEndFrames;
  if (hasMotion) {
    rangeStartFrames.reserve(ranges.size());
    rangeEndFrames.reserve(ranges.size());
    for (const auto& range : ranges) {
      const int64_t startFrame = frameIndexAtUs(range.startUs, frameRate);
      const int64_t endFrame = frameIndexAtUs(range.endUs, frameRate);
      if (startFrame < 0 || endFrame <= startFrame) {
        setError(error,
                 "Smooth cut requires source ranges aligned to video frames.");
        return false;
      }
      rangeStartFrames.push_back(startFrame);
      rangeEndFrames.push_back(endFrame);
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
        frameOffsetUs(transition.outgoingFrames, frameRate);
    const int64_t durationUs = frameOffsetUs(durationFrames, frameRate);
    const int64_t outgoingAnchorFrame =
        rangeEndFrames[cutIndex] - transition.outgoingFrames;
    const int64_t incomingAnchorFrame =
        rangeStartFrames[cutIndex + 1] + transition.incomingFrames;
    if (outgoingAnchorFrame - 1 < rangeStartFrames[cutIndex] ||
        incomingAnchorFrame + 2 > rangeEndFrames[cutIndex + 1]) {
      setError(error,
               "A clip next to smooth cut " +
                   std::to_string(cutIndex + 1) +
                   " is too short for its " +
                   std::to_string(durationFrames) +
                   "-frame transition.");
      return false;
    }
    const int64_t outgoingAnchorUs =
        frameOffsetUs(outgoingAnchorFrame, frameRate);
    const int64_t incomingAnchorUs =
        frameOffsetUs(incomingAnchorFrame, frameRate);
    next.cuts[cutIndex] = MotionTransitionWindow{
        cutIndex,
        transition.outgoingFrames,
        transition.incomingFrames,
        durationFrames,
        durationUs,
        presentationCutUs - outgoingUs,
        frameOffsetUs(outgoingAnchorFrame - 1, frameRate),
        outgoingAnchorUs,
        incomingAnchorUs,
        frameOffsetUs(incomingAnchorFrame + 2, frameRate),
        outgoingAnchorFrame - 1,
        outgoingAnchorFrame,
        incomingAnchorFrame,
        incomingAnchorFrame + 2,
    };
  }

  for (size_t clipIndex = 0; clipIndex < ranges.size(); ++clipIndex) {
    int64_t startUs = ranges[clipIndex].startUs;
    int64_t endUs = ranges[clipIndex].endUs;
    int64_t startFrame = hasMotion ? rangeStartFrames[clipIndex] : -1;
    int64_t endFrame = hasMotion ? rangeEndFrames[clipIndex] : -1;
    if (clipIndex > 0 && next.cuts[clipIndex - 1]) {
      startUs += frameOffsetUs(next.cuts[clipIndex - 1]->incomingFrames,
                               frameRate);
      startFrame += next.cuts[clipIndex - 1]->incomingFrames;
    }
    if (clipIndex < next.cuts.size() && next.cuts[clipIndex]) {
      endUs -= frameOffsetUs(next.cuts[clipIndex]->outgoingFrames,
                             frameRate);
      endFrame -= next.cuts[clipIndex]->outgoingFrames;
    }
    if (endUs <= startUs || (hasMotion && endFrame <= startFrame)) {
      setError(error,
               "Smooth-cut overlaps meet inside clip " +
                   std::to_string(clipIndex + 1) + ".");
      return false;
    }
    next.clips.push_back({startUs, endUs, startFrame, endFrame});
  }

  *plan = std::move(next);
  return true;
}

std::string buildProgramFilterDescription(const RenderPlan& plan,
                                          AVPixelFormat outputFormat,
                                          std::string* error) {
  if (plan.clips.empty() || plan.cuts.size() + 1 != plan.clips.size()) {
    setError(error, "The video composition render plan has an invalid shape.");
    return {};
  }
  const char* formatName = pixelFormatName(outputFormat, error);
  if (!formatName) return {};

  size_t branchCount = plan.clips.size();
  for (const auto& cut : plan.cuts) {
    if (cut) branchCount += 2;
  }
  std::string description = "[src]split=" + std::to_string(branchCount);
  for (size_t clip = 0; clip < plan.clips.size(); ++clip) {
    description += "[raw_m" + std::to_string(clip) + "]";
  }
  for (size_t cut = 0; cut < plan.cuts.size(); ++cut) {
    if (!plan.cuts[cut]) continue;
    description += "[raw_l" + std::to_string(cut) + "]";
    description += "[raw_r" + std::to_string(cut) + "]";
  }
  description += ";";

  for (size_t clip = 0; clip < plan.clips.size(); ++clip) {
    const ClipWindow& window = plan.clips[clip];
    description += "[raw_m" + std::to_string(clip) + "]trim=start_";
    if (plan.hasMotionTransitions()) {
      description += "frame=" + std::to_string(window.sourceStartFrame) +
                     ":end_frame=" +
                     std::to_string(window.sourceEndFrame);
    } else {
      description += "pts=" + std::to_string(window.sourceStartUs) +
                     ":end_pts=" + std::to_string(window.sourceEndUs);
    }
    description += ",setpts=PTS-STARTPTS[m" + std::to_string(clip) + "];";
  }
  for (size_t cut = 0; cut < plan.cuts.size(); ++cut) {
    if (!plan.cuts[cut]) continue;
    if (!appendMotionTransition(
            &description, plan, *plan.cuts[cut],
            "[raw_l" + std::to_string(cut) + "]",
            "[raw_r" + std::to_string(cut) + "]",
            "[s" + std::to_string(cut) + "]", outputFormat, error)) {
      return {};
    }
  }

  size_t partCount = plan.clips.size();
  for (const auto& cut : plan.cuts) {
    if (cut) ++partCount;
  }
  for (size_t clip = 0; clip < plan.clips.size(); ++clip) {
    description += "[m" + std::to_string(clip) + "]";
    if (clip < plan.cuts.size() && plan.cuts[clip]) {
      description += "[s" + std::to_string(clip) + "]";
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
