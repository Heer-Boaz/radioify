#include "playback/video/edit/export_render_plan.h"

extern "C" {
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <limits>
#include <utility>

namespace playback_video_edit::detail {
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

}  // namespace

bool VideoRenderPlan::hasSmoothCuts() const {
  return std::any_of(cuts.begin(), cuts.end(),
                     [](const auto& cut) { return cut.has_value(); });
}

bool buildVideoRenderPlan(const DecisionList& decisions, AVRational frameRate,
                          VideoRenderPlan* plan, std::string* error) {
  if (!plan || decisions.keptRanges.empty() ||
      !decisions.hasValidShape()) {
    setError(error, "The edit decision list is incomplete.");
    return false;
  }

  VideoRenderPlan next;
  next.frameRate = frameRate;
  next.clips.reserve(decisions.keptRanges.size());
  next.cuts.resize(decisions.cutTransitions.size());
  const bool hasSmooth = std::any_of(
      decisions.cutTransitions.begin(), decisions.cutTransitions.end(),
      [](const CutTransition& transition) {
        return transition.kind == CutTransitionKind::Smooth;
      });
  if (hasSmooth &&
      (frameRate.num <= 0 || frameRate.den <= 0 ||
       av_q2d(frameRate) < 1.0 || av_q2d(frameRate) > 240.0)) {
    setError(error,
             "Smooth cut requires a stable sequence frame rate between 1 "
             "and 240 fps.");
    return false;
  }
  if (hasSmooth) {
    next.frameDurationUs = av_rescale_q_rnd(
        1, av_inv_q(frameRate), kMicrosecondTimeBase,
        static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    if (next.frameDurationUs <= 0) {
      setError(error, "Smooth cut could not resolve the sequence timebase.");
      return false;
    }
  }

  for (size_t cutIndex = 0; cutIndex < decisions.cutTransitions.size();
       ++cutIndex) {
    const CutTransition& transition = decisions.cutTransitions[cutIndex];
    if (transition.kind != CutTransitionKind::Smooth) continue;
    if (next.frameDurationUs >
        (std::numeric_limits<int64_t>::max)() /
            transition.durationFrames) {
      setError(error, "Smooth cut duration exceeds the render timebase.");
      return false;
    }
    const int64_t durationUs =
        next.frameDurationUs * transition.durationFrames;
    const int64_t outgoingHalfUs = durationUs / 2;
    const int64_t incomingHalfUs = durationUs - outgoingHalfUs;
    const SourceRange& outgoing = decisions.keptRanges[cutIndex];
    const SourceRange& incoming = decisions.keptRanges[cutIndex + 1];
    if (outgoingHalfUs < next.frameDurationUs ||
        incomingHalfUs < next.frameDurationUs ||
        outgoing.durationUs() <= outgoingHalfUs ||
        incoming.durationUs() <= incomingHalfUs) {
      setError(error,
               "A clip next to smooth cut " +
                   std::to_string(cutIndex + 1) +
                   " is too short for its " +
                   std::to_string(transition.durationFrames) +
                   "-frame transition.");
      return false;
    }
    next.cuts[cutIndex] = SmoothCutWindow{
        cutIndex,
        transition.durationFrames,
        durationUs,
        outgoing.endUs - outgoingHalfUs,
        incoming.startUs + incomingHalfUs - next.frameDurationUs,
    };
  }

  for (size_t clipIndex = 0; clipIndex < decisions.keptRanges.size();
       ++clipIndex) {
    const SourceRange& source = decisions.keptRanges[clipIndex];
    int64_t startUs = source.startUs;
    int64_t endUs = source.endUs;
    if (clipIndex > 0 && next.cuts[clipIndex - 1]) {
      const int64_t durationUs = next.cuts[clipIndex - 1]->durationUs;
      startUs += durationUs - durationUs / 2;
    }
    if (clipIndex < next.cuts.size() && next.cuts[clipIndex]) {
      endUs -= next.cuts[clipIndex]->durationUs / 2;
    }
    if (endUs <= startUs ||
        (hasSmooth && endUs - startUs < next.frameDurationUs)) {
      setError(error,
               "Smooth-cut windows overlap inside clip " +
                   std::to_string(clipIndex + 1) + ".");
      return false;
    }
    next.clips.push_back({startUs, endUs});
  }

  *plan = std::move(next);
  return true;
}

std::string buildVideoFilterDescription(const VideoRenderPlan& plan,
                                        AVPixelFormat outputFormat,
                                        std::string* error) {
  if (plan.clips.empty() || plan.cuts.size() + 1 != plan.clips.size()) {
    setError(error, "The video render plan has an invalid shape.");
    return {};
  }
  const char* pixelFormat = av_get_pix_fmt_name(outputFormat);
  if (!pixelFormat) {
    setError(error, "The preserving video pixel format is not named.");
    return {};
  }

  size_t branchCount = plan.clips.size();
  for (const auto& cut : plan.cuts) {
    if (cut) branchCount += 2;
  }
  std::string description =
      "[src]split=" + std::to_string(branchCount);
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
    const RenderClipWindow& window = plan.clips[clip];
    description += "[raw_m" + std::to_string(clip) +
                   "]trim=start_pts=" + std::to_string(window.sourceStartUs) +
                   ":end_pts=" + std::to_string(window.sourceEndUs) +
                   ",setpts=PTS-STARTPTS[m" + std::to_string(clip) + "];";
  }

  const std::string frameRate = std::to_string(plan.frameRate.num) + "/" +
                                std::to_string(plan.frameRate.den);
  for (size_t cut = 0; cut < plan.cuts.size(); ++cut) {
    if (!plan.cuts[cut]) continue;
    const SmoothCutWindow& window = *plan.cuts[cut];
    const std::string number = std::to_string(cut);
    int64_t outgoingEndUs = 0;
    int64_t incomingEndUs = 0;
    int64_t interpolationEndUs = 0;
    if (!addWithoutOverflow(window.outgoingFrameUs, plan.frameDurationUs,
                            &outgoingEndUs) ||
        !addWithoutOverflow(window.incomingFrameUs, plan.frameDurationUs,
                            &incomingEndUs)) {
      setError(error, "Smooth-cut sample timestamp overflowed.");
      return {};
    }
    if (!addWithoutOverflow(window.durationUs, window.durationUs,
                            &interpolationEndUs)) {
      setError(error, "Smooth-cut interpolation timestamp overflowed.");
      return {};
    }
    description +=
        "[raw_l" + number + "]trim=start_pts=" +
        std::to_string(window.outgoingFrameUs) + ":end_pts=" +
        std::to_string(outgoingEndUs) +
        ",setpts=PTS-STARTPTS,trim=end_frame=1,split=2[l" + number +
        "a][l" + number + "b];";
    description +=
        "[raw_r" + number + "]trim=start_pts=" +
        std::to_string(window.incomingFrameUs) + ":end_pts=" +
        std::to_string(incomingEndUs) +
        ",setpts=PTS-STARTPTS,trim=end_frame=1,split=2[r" + number +
        "a][r" + number + "b];";
    description +=
        "[l" + number + "a][l" + number + "b][r" + number + "a][r" +
        number + "b]concat=n=4:v=1:a=0,settb=1/1000000,setpts=N*" +
        std::to_string(window.durationUs) + ",minterpolate=fps=" + frameRate +
        ":mi_mode=mci:mc_mode=aobmc:me_mode=bidir:vsbmc=1:scd=none," +
        "settb=1/1000000,trim=start_pts=" +
        std::to_string(window.durationUs) + ":end_pts=" +
        std::to_string(interpolationEndUs) +
        ",setpts=PTS-STARTPTS[s" + number + "];";
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
                 ":v=1:a=0[cat];[cat]format=pix_fmts=" + pixelFormat +
                 "[out]";
  return description;
}

}  // namespace playback_video_edit::detail
