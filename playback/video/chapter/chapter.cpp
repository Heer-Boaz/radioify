#include "playback/video/chapter/chapter.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace playback_video_chapters {
namespace {

void setError(std::string* error, std::string value) {
  if (error) *error = std::move(value);
}

}  // namespace

bool validatePartition(std::int64_t durationUs,
                       const std::vector<Chapter>& chapters,
                       std::string* error) {
  if (error) error->clear();
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
  for (const Chapter& chapter : chapters) {
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

const Chapter* chapterAt(const Snapshot& snapshot, std::int64_t positionUs) {
  if (!snapshot.ready() || snapshot.durationUs <= 0) return nullptr;
  positionUs = std::clamp(positionUs, std::int64_t{0},
                          snapshot.durationUs - 1);
  const auto after = std::upper_bound(
      snapshot.chapters.begin(), snapshot.chapters.end(), positionUs,
      [](std::int64_t value, const Chapter& chapter) {
        return value < chapter.startUs;
      });
  if (after == snapshot.chapters.begin()) return nullptr;
  const Chapter& chapter = *(after - 1);
  return positionUs < chapter.endUs ? &chapter : nullptr;
}

MarkerProjection projectMarkers(const Snapshot& snapshot, int units) {
  MarkerProjection projection;
  if (!snapshot.ready() || snapshot.durationUs <= 0 || units <= 0) {
    return projection;
  }
  std::vector<int> counts(static_cast<std::size_t>(units), 0);
  for (std::size_t index = 1; index < snapshot.chapters.size(); ++index) {
    const double ratio =
        static_cast<double>(snapshot.chapters[index].startUs) /
        static_cast<double>(snapshot.durationUs);
    const int cell = std::clamp(
        static_cast<int>(std::llround(
            ratio * static_cast<double>(std::max(0, units - 1)))),
        0, units - 1);
    ++counts[static_cast<std::size_t>(cell)];
  }
  for (int cell = 0; cell < units; ++cell) {
    const int count = counts[static_cast<std::size_t>(cell)];
    if (count <= 0) continue;
    projection.boundaryCells.push_back(cell);
    if (count > 1) projection.collisionCells.push_back(cell);
  }
  return projection;
}

const char* analysisStateLabel(AnalysisState state) {
  switch (state) {
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
      return "Chapters ready";
    case AnalysisState::Unsupported:
      return "Chapter analysis unavailable";
    case AnalysisState::Failed:
      return "Chapter analysis failed";
  }
  return "Chapter analysis unavailable";
}

}  // namespace playback_video_chapters
