#include "playback/video/sequence.h"

#include <algorithm>
#include <limits>

namespace playback_video_sequence {
namespace {

bool validRange(SourceRange range, int64_t sourceDurationUs) {
  return range.startUs >= 0 && range.endUs > range.startUs &&
         range.endUs <= sourceDurationUs;
}

bool canAdd(int64_t lhs, int64_t rhs) {
  return rhs >= 0 && lhs <= (std::numeric_limits<int64_t>::max)() - rhs;
}

}  // namespace

std::optional<Timeline> Timeline::create(
    int64_t sourceDurationUs, const std::vector<SourceRange>& ranges) {
  if (sourceDurationUs <= 0 || ranges.empty()) return std::nullopt;

  Timeline timeline;
  timeline.sourceDurationUs_ = sourceDurationUs;
  timeline.clips_.reserve(ranges.size());
  int64_t previousEndUs = -1;
  int64_t presentationUs = 0;
  for (const SourceRange range : ranges) {
    if (!validRange(range, sourceDurationUs) ||
        range.startUs < previousEndUs ||
        !canAdd(presentationUs, range.durationUs())) {
      return std::nullopt;
    }
    timeline.clips_.push_back(Clip{range, presentationUs});
    presentationUs += range.durationUs();
    previousEndUs = range.endUs;
  }
  timeline.durationUs_ = presentationUs;
  return timeline;
}

bool Timeline::isIdentity() const {
  return clips_.size() == 1 && clips_.front().source.startUs == 0 &&
         clips_.front().source.endUs == sourceDurationUs_;
}

Point Timeline::pointAt(int64_t presentationUs) const {
  presentationUs = std::clamp(presentationUs, int64_t{0}, durationUs_);
  if (presentationUs == durationUs_) {
    const size_t index = clips_.size() - 1;
    return Point{index, clips_[index].source.endUs, durationUs_};
  }

  const auto found = std::upper_bound(
      clips_.begin(), clips_.end(), presentationUs,
      [](int64_t value, const Clip& clip) {
        return value < clip.presentationEndUs();
      });
  const size_t index = static_cast<size_t>(found - clips_.begin());
  const Clip& clip = clips_[index];
  return Point{index,
               clip.source.startUs + presentationUs -
                   clip.presentationStartUs,
               presentationUs};
}

std::optional<Point> Timeline::pointForSource(int64_t sourceUs,
                                               SourceBias bias) const {
  sourceUs = std::clamp(sourceUs, int64_t{0}, sourceDurationUs_);
  for (size_t index = 0; index < clips_.size(); ++index) {
    const Clip& clip = clips_[index];
    if (sourceUs >= clip.source.startUs && sourceUs < clip.source.endUs) {
      return Point{index, sourceUs,
                   clip.presentationStartUs + sourceUs - clip.source.startUs};
    }
    if (sourceUs < clip.source.startUs) {
      if (bias == SourceBias::Forward || index == 0) {
        return Point{index, clip.source.startUs, clip.presentationStartUs};
      }
      const Clip& previous = clips_[index - 1];
      return Point{index - 1, previous.source.endUs,
                   previous.presentationEndUs()};
    }
  }

  const size_t index = clips_.size() - 1;
  const Clip& clip = clips_[index];
  return Point{index, clip.source.endUs, clip.presentationEndUs()};
}

}  // namespace playback_video_sequence
