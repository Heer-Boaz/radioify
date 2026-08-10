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

auto firstClipEndingAfter(const std::vector<Clip>& clips, int64_t sourceUs) {
  return std::upper_bound(
      clips.begin(), clips.end(), sourceUs,
      [](int64_t value, const Clip& clip) {
        return value < clip.source.endUs;
      });
}

std::optional<size_t> firstClipIntersectingSource(
    const std::vector<Clip>& clips, SourceRange source) {
  if (source.startUs < 0 || source.endUs <= source.startUs) {
    return std::nullopt;
  }
  const auto found = firstClipEndingAfter(clips, source.startUs);
  if (found == clips.end() || found->source.startUs >= source.endUs) {
    return std::nullopt;
  }
  return static_cast<size_t>(found - clips.begin());
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

std::optional<size_t> Timeline::clipIndexAtSource(int64_t sourceUs) const {
  const auto found = firstClipEndingAfter(clips_, sourceUs);
  if (found == clips_.end() || sourceUs < found->source.startUs) {
    return std::nullopt;
  }
  return static_cast<size_t>(found - clips_.begin());
}

std::optional<Point> Timeline::pointForSource(int64_t sourceUs,
                                               SourceBias bias) const {
  sourceUs = std::clamp(sourceUs, int64_t{0}, sourceDurationUs_);
  const auto found = firstClipEndingAfter(clips_, sourceUs);
  if (found != clips_.end()) {
    const size_t index = static_cast<size_t>(found - clips_.begin());
    if (sourceUs >= found->source.startUs) {
      return Point{index, sourceUs,
                   found->presentationStartUs + sourceUs -
                       found->source.startUs};
    }
    if (bias == SourceBias::Forward || index == 0) {
      return Point{index, found->source.startUs, found->presentationStartUs};
    }
    const Clip& previous = clips_[index - 1];
    return Point{index - 1, previous.source.endUs,
                 previous.presentationEndUs()};
  }

  const size_t index = clips_.size() - 1;
  const Clip& clip = clips_[index];
  return Point{index, clip.source.endUs, clip.presentationEndUs()};
}

std::optional<FrameMapping> Timeline::mapFrame(
    int64_t sourcePtsUs, int64_t sourceDurationUs) const {
  if (sourceDurationUs <= 0) return std::nullopt;
  const auto clipIndex = clipIndexAtSource(sourcePtsUs);
  if (!clipIndex) return std::nullopt;
  const Clip& clip = clips_[*clipIndex];
  return FrameMapping{
      clip.presentationStartUs + sourcePtsUs - clip.source.startUs,
      (std::min)(sourceDurationUs, clip.source.endUs - sourcePtsUs)};
}

std::optional<AudioSlice> Timeline::sliceAudio(
    int64_t sourcePtsUs, uint64_t frameCount, uint32_t sampleRate) const {
  if (sourcePtsUs < 0 || frameCount == 0 || sampleRate == 0 ||
      frameCount > (std::numeric_limits<uint64_t>::max)() / 1000000ULL) {
    return std::nullopt;
  }
  const uint64_t durationUs =
      frameCount * 1000000ULL / static_cast<uint64_t>(sampleRate);
  if (durationUs > static_cast<uint64_t>(
                       (std::numeric_limits<int64_t>::max)() - sourcePtsUs)) {
    return std::nullopt;
  }
  const int64_t sourceEndUs = sourcePtsUs + static_cast<int64_t>(durationUs);
  const auto clipIndex =
      firstClipIntersectingSource(clips_, {sourcePtsUs, sourceEndUs});
  if (!clipIndex) return std::nullopt;
  const Clip& clip = clips_[*clipIndex];
  const int64_t startUs = (std::max)(sourcePtsUs, clip.source.startUs);
  const int64_t endUs = (std::min)(sourceEndUs, clip.source.endUs);
  const auto frameAtOrAfter = [sampleRate](int64_t deltaUs) {
    if (deltaUs <= 0) return uint64_t{0};
    const uint64_t scaled =
        static_cast<uint64_t>(deltaUs) * static_cast<uint64_t>(sampleRate);
    return scaled / 1000000ULL + (scaled % 1000000ULL != 0 ? 1 : 0);
  };
  const uint64_t first =
      (std::min)(frameCount, frameAtOrAfter(startUs - sourcePtsUs));
  const uint64_t end =
      (std::min)(frameCount, frameAtOrAfter(endUs - sourcePtsUs));
  if (end <= first) return std::nullopt;

  const int64_t firstSourceUs =
      sourcePtsUs + static_cast<int64_t>(
                        first * 1000000ULL /
                        static_cast<uint64_t>(sampleRate));
  return AudioSlice{
      first, end - first,
      clip.presentationStartUs +
          (std::max)(int64_t{0}, firstSourceUs - clip.source.startUs)};
}

}  // namespace playback_video_sequence
