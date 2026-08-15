#include "playback/video/frame_step_prefetch.h"

#include <algorithm>
#include <limits>

#include "playback/video/sequence.h"

namespace playback_video_frame_step_prefetch {

std::optional<Request> mapRequestToTimeline(
    const playback_video_sequence::Timeline& timeline, Request request) {
  request.joinContinuity = JoinContinuity::Source;
  request.reachesTimelineBoundary = false;
  const auto& clips = timeline.clips();
  const auto joinClipIndex =
      timeline.clipIndexAtSource(request.join.sourcePtsUs);
  if (!joinClipIndex) return std::nullopt;

  const bool previous =
      request.direction == playback_video_frame_step::Direction::Previous;
  const playback_video_sequence::Clip& joinClip = clips[*joinClipIndex];
  const int64_t joinDurationUs =
      (std::max)(int64_t{1}, request.join.durationUs);
  const int64_t joinEndUs =
      request.join.sourcePtsUs <=
              (std::numeric_limits<int64_t>::max)() - joinDurationUs
          ? request.join.sourcePtsUs + joinDurationUs
          : request.join.sourcePtsUs;

  size_t targetClipIndex = *joinClipIndex;
  if (previous && targetClipIndex > 0 &&
      request.join.sourcePtsUs - joinClip.source.startUs < joinDurationUs) {
    --targetClipIndex;
    request.joinContinuity = JoinContinuity::Presentation;
  } else if (!previous && targetClipIndex + 1 < clips.size() &&
             joinEndUs >= joinClip.source.endUs) {
    // Timeline::mapFrame clips the presented duration at the half-open source
    // boundary. Crossing on that exact end avoids guessing adjacency from a
    // nominal duration, whose microsecond rounding can differ by one tick.
    ++targetClipIndex;
    request.joinContinuity = JoinContinuity::Presentation;
  }

  const playback_video_sequence::Clip& sourceClip = clips[targetClipIndex];
  const int64_t presentationStartUs =
      (std::max)(request.rangeStartUs, sourceClip.presentationStartUs);
  const int64_t presentationEndUs =
      (std::min)(request.rangeEndUs, sourceClip.presentationEndUs());
  if (presentationEndUs <= presentationStartUs) return std::nullopt;

  request.sourceRangeStartUs =
      sourceClip.source.startUs +
      (presentationStartUs - sourceClip.presentationStartUs);
  request.sourceRangeEndUs =
      sourceClip.source.startUs +
      (presentationEndUs - sourceClip.presentationStartUs);
  request.presentationOffsetUs =
      sourceClip.presentationStartUs - sourceClip.source.startUs;
  request.reachesTimelineBoundary =
      previous ? targetClipIndex == 0 &&
                     presentationStartUs == sourceClip.presentationStartUs
               : targetClipIndex + 1 == clips.size() &&
                     presentationEndUs == sourceClip.presentationEndUs();

  const bool presentationJoin =
      request.joinContinuity == JoinContinuity::Presentation;
  if (!presentationJoin &&
      (previous
           ? request.sourceRangeStartUs >= request.join.sourcePtsUs
           : joinEndUs >= request.sourceRangeEndUs)) {
    return std::nullopt;
  }
  return request;
}

}  // namespace playback_video_frame_step_prefetch
