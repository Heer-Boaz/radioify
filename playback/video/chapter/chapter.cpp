#include "playback/video/chapter/chapter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "core/utf8.h"

namespace playback_video_chapters {
namespace {

void setError(std::string *error, std::string value) {
  if (error)
    *error = std::move(value);
}

std::string normalizedMetadata(std::string_view value) {
  std::string normalized;
  normalized.reserve(value.size());
  bool pendingSpace = false;
  for (unsigned char ch : value) {
    if (std::isspace(ch)) {
      pendingSpace = !normalized.empty();
      continue;
    }
    if (pendingSpace)
      normalized.push_back(' ');
    pendingSpace = false;
    normalized.push_back(static_cast<char>(std::tolower(ch)));
  }
  return normalized;
}

bool validPublishedText(std::string_view value, std::size_t maximumBytes,
                        bool requireSentenceEnd = false) {
  if (value.empty() || value.size() > maximumBytes || !isValidUtf8(value)) {
    return false;
  }
  for (unsigned char ch : value) {
    if (ch < 0x20 || ch == 0x7f)
      return false;
  }
  if (normalizedMetadata(value).empty())
    return false;
  if (!requireSentenceEnd)
    return true;
  const char final = value.back();
  return final == '.' || final == '!' || final == '?';
}

} // namespace

bool validatePartition(std::int64_t durationUs,
                       const std::vector<Chapter> &chapters,
                       std::string *error) {
  if (error)
    error->clear();
  if (durationUs <= 0) {
    setError(error, "Chapter analysis requires a positive video duration.");
    return false;
  }
  if (chapters.empty()) {
    setError(error, "Chapter analysis returned no chapters.");
    return false;
  }
  if (chapters.front().startUs != 0) {
    setError(error, "The first chapter does not begin at the video start.");
    return false;
  }

  std::unordered_set<std::uint64_t> ids;
  std::int64_t expectedStartUs = 0;
  for (const Chapter &chapter : chapters) {
    if (chapter.id == 0 || !ids.insert(chapter.id).second ||
        chapter.title.empty()) {
      setError(error, "A chapter has an invalid identity or title.");
      return false;
    }
    if (chapter.startUs != expectedStartUs ||
        chapter.endUs <= chapter.startUs || chapter.endUs > durationUs) {
      setError(error, "The chapters do not form a contiguous video timeline.");
      return false;
    }
    expectedStartUs = chapter.endUs;
  }
  if (expectedStartUs != durationUs) {
    setError(error, "The final chapter does not reach the video end.");
    return false;
  }
  return true;
}

bool validateAutomaticPartition(std::int64_t durationUs,
                                const std::vector<Chapter> &chapters,
                                std::string *error) {
  if (!validatePartition(durationUs, chapters, error))
    return false;
  if (chapters.size() < kMinimumAutomaticChapterCount ||
      chapters.size() > kMaximumAutomaticChapterCount) {
    setError(error, "Automatic chapter analysis must produce between one and "
                    "one hundred chapters.");
    return false;
  }
  return true;
}

bool validateAutomaticAnalysis(std::int64_t durationUs,
                               const std::vector<Chapter> &chapters,
                               std::string *error) {
  if (!validateAutomaticPartition(durationUs, chapters, error))
    return false;
  for (std::size_t index = 0; index < chapters.size(); ++index) {
    const Chapter &chapter = chapters[index];
    if (!validPublishedText(chapter.title, kMaximumAutomaticTitleBytes)) {
      setError(error, "Automatic chapter analysis returned an invalid title "
                      "for chapter " +
                          std::to_string(index + 1) + ".");
      return false;
    }
  }
  return true;
}

const Chapter *chapterAt(const Snapshot &snapshot, std::int64_t positionUs) {
  if (!snapshot.ready() || snapshot.durationUs <= 0)
    return nullptr;
  positionUs = std::clamp(positionUs, std::int64_t{0}, snapshot.durationUs - 1);
  const auto after = std::upper_bound(
      snapshot.chapters.begin(), snapshot.chapters.end(), positionUs,
      [](std::int64_t value, const Chapter &chapter) {
        return value < chapter.startUs;
      });
  if (after == snapshot.chapters.begin())
    return nullptr;
  const Chapter &chapter = *(after - 1);
  return positionUs < chapter.endUs ? &chapter : nullptr;
}

std::optional<std::int64_t> navigationTarget(const Snapshot &snapshot,
                                             std::int64_t positionUs,
                                             NavigationDirection direction) {
  const Chapter *current = chapterAt(snapshot, positionUs);
  if (!current)
    return std::nullopt;
  auto found = std::find_if(
      snapshot.chapters.begin(), snapshot.chapters.end(),
      [&](const Chapter &chapter) { return chapter.id == current->id; });
  if (found == snapshot.chapters.end())
    return std::nullopt;
  if (direction == NavigationDirection::Previous) {
    constexpr std::int64_t kRestartChapterThresholdUs = 5'000'000;
    if (positionUs - found->startUs > kRestartChapterThresholdUs)
      return found->startUs;
    return found == snapshot.chapters.begin()
               ? std::nullopt
               : std::optional<std::int64_t>((found - 1)->startUs);
  }
  ++found;
  return found == snapshot.chapters.end()
             ? std::nullopt
             : std::optional<std::int64_t>(found->startUs);
}

namespace {

MarkerProjection projectMarkerTimes(const std::vector<std::int64_t> &timesUs,
                                    std::int64_t durationUs, int units) {
  MarkerProjection projection;
  if (durationUs <= 0 || units <= 0)
    return projection;
  std::vector<int> counts(static_cast<std::size_t>(units), 0);
  for (const std::int64_t timeUs : timesUs) {
    if (timeUs <= 0 || timeUs >= durationUs)
      continue;
    const double ratio =
        static_cast<double>(timeUs) / static_cast<double>(durationUs);
    const int cell =
        std::clamp(static_cast<int>(std::llround(
                       ratio * static_cast<double>(std::max(0, units - 1)))),
                   0, units - 1);
    ++counts[static_cast<std::size_t>(cell)];
  }
  for (int cell = 0; cell < units; ++cell) {
    const int count = counts[static_cast<std::size_t>(cell)];
    if (count <= 0)
      continue;
    projection.boundaryCells.push_back(cell);
    if (count > 1)
      projection.collisionCells.push_back(cell);
  }
  return projection;
}

} // namespace

MarkerProjection projectMarkers(const Snapshot &snapshot, int units) {
  if (!snapshot.ready())
    return {};
  std::vector<std::int64_t> timesUs;
  timesUs.reserve(snapshot.chapters.size());
  for (std::size_t index = 1; index < snapshot.chapters.size(); ++index)
    timesUs.push_back(snapshot.chapters[index].startUs);
  return projectMarkerTimes(timesUs, snapshot.durationUs, units);
}

std::optional<Snapshot> projectToPresentationTimeline(
    const Snapshot &snapshot, std::int64_t presentationDurationUs,
    const std::vector<MarkerTimelineSegment> &segments) {
  if (!snapshot.ready() || presentationDurationUs <= 0 || segments.empty())
    return std::nullopt;

  std::int64_t expectedPresentationUs = 0;
  for (const MarkerTimelineSegment &segment : segments) {
    if (segment.sourceStartUs < 0 ||
        segment.sourceEndUs <= segment.sourceStartUs ||
        segment.sourceEndUs > snapshot.durationUs ||
        segment.presentationStartUs != expectedPresentationUs) {
      return std::nullopt;
    }
    expectedPresentationUs += segment.sourceEndUs - segment.sourceStartUs;
  }
  if (expectedPresentationUs != presentationDurationUs)
    return std::nullopt;

  Snapshot projected = snapshot;
  projected.durationUs = presentationDurationUs;
  projected.chapters.clear();
  projected.chapters.reserve(snapshot.chapters.size());
  for (const MarkerTimelineSegment &segment : segments) {
    for (const Chapter &sourceChapter : snapshot.chapters) {
      const std::int64_t sourceStartUs =
          std::max(sourceChapter.startUs, segment.sourceStartUs);
      const std::int64_t sourceEndUs =
          std::min(sourceChapter.endUs, segment.sourceEndUs);
      if (sourceEndUs <= sourceStartUs)
        continue;
      Chapter chapter = sourceChapter;
      chapter.startUs = segment.presentationStartUs + sourceStartUs -
                        segment.sourceStartUs;
      chapter.endUs = segment.presentationStartUs + sourceEndUs -
                      segment.sourceStartUs;
      if (!projected.chapters.empty() &&
          projected.chapters.back().id == chapter.id &&
          projected.chapters.back().endUs == chapter.startUs) {
        projected.chapters.back().endUs = chapter.endUs;
      } else {
        projected.chapters.push_back(std::move(chapter));
      }
    }
  }
  std::string error;
  if (!validatePartition(projected.durationUs, projected.chapters, &error))
    return std::nullopt;
  return projected;
}

const char *analysisStateLabel(AnalysisState state) {
  switch (state) {
  case AnalysisState::Disabled:
    return "Automatic chapters disabled";
  case AnalysisState::CheckingSupport:
    return "Checking chapter support";
  case AnalysisState::SetupRequired:
    return "Chapter model setup required";
  case AnalysisState::Installing:
    return "Installing chapter model";
  case AnalysisState::WaitingForPlayback:
    return "Chapter analysis waiting for playback";
  case AnalysisState::Analyzing:
    return "Analyzing video";
  case AnalysisState::Ready:
    return "Chapter analysis complete";
  case AnalysisState::Unsupported:
    return "Chapter analysis unavailable";
  case AnalysisState::Failed:
    return "Chapter analysis failed";
  }
  return "Chapter analysis unavailable";
}

} // namespace playback_video_chapters
