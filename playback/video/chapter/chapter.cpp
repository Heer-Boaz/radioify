#include "playback/video/chapter/chapter.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <utility>

#include "core/utf8.h"

namespace playback_video_chapters {
namespace {

void setError(std::string* error, std::string value) {
  if (error) *error = std::move(value);
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
    if (pendingSpace) normalized.push_back(' ');
    pendingSpace = false;
    normalized.push_back(static_cast<char>(std::tolower(ch)));
  }
  return normalized;
}

std::size_t wordCount(std::string_view value) {
  std::size_t count = 0;
  bool inWord = false;
  for (unsigned char ch : value) {
    if (std::isspace(ch)) {
      inWord = false;
    } else if (!inWord) {
      ++count;
      inWord = true;
    }
  }
  return count;
}

bool validPublishedText(std::string_view value, std::size_t maximumBytes,
                        std::size_t maximumWords, bool completeSentence) {
  if (value.empty() || value.size() > maximumBytes || !isValidUtf8(value)) {
    return false;
  }
  for (unsigned char ch : value) {
    if (ch < 0x20 || ch == 0x7f) return false;
  }
  if (wordCount(value) > maximumWords || normalizedMetadata(value).empty()) {
    return false;
  }
  if (!completeSentence) return true;
  const char final = value.back();
  return final == '.' || final == '!' || final == '?';
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

bool validateAutomaticPartition(std::int64_t durationUs,
                                const std::vector<Chapter>& chapters,
                                std::string* error) {
  if (!validatePartition(durationUs, chapters, error)) return false;
  if (chapters.size() < kMinimumAutomaticChapterCount ||
      chapters.size() > kMaximumAutomaticChapterCount) {
    setError(error,
             "Automatic chapter analysis must produce between three and "
             "twelve chapters.");
    return false;
  }
  for (const Chapter& chapter : chapters) {
    if (chapter.endUs - chapter.startUs < kMinimumAutomaticChapterDurationUs) {
      setError(error,
               "Automatic chapter analysis produced a chapter shorter than "
               "ten seconds.");
      return false;
    }
  }
  return true;
}

bool validateAutomaticAnalysis(std::int64_t durationUs,
                               std::string_view overview,
                               const std::vector<Chapter>& chapters,
                               std::string* error) {
  if (!validateAutomaticPartition(durationUs, chapters, error)) return false;
  if (!validPublishedText(overview, kMaximumAutomaticOverviewBytes,
                          kMaximumAutomaticOverviewWords, true)) {
    setError(error, "Automatic chapter analysis returned an invalid overview.");
    return false;
  }
  std::optional<std::pair<std::string, std::string>> firstMetadata;
  bool allMetadataEqual = true;
  for (const Chapter& chapter : chapters) {
    if (!validPublishedText(chapter.title, kMaximumAutomaticTitleBytes,
                            kMaximumAutomaticTitleWords, false) ||
        !validPublishedText(chapter.summary, kMaximumAutomaticSummaryBytes,
                            kMaximumAutomaticSummaryWords, true)) {
      setError(error, "Automatic chapter analysis returned invalid metadata.");
      return false;
    }
    const std::string title = normalizedMetadata(chapter.title);
    const std::string summary = normalizedMetadata(chapter.summary);
    const std::pair<std::string, std::string> metadata{title, summary};
    if (!firstMetadata) {
      firstMetadata = metadata;
    } else if (metadata != *firstMetadata) {
      allMetadataEqual = false;
    }
  }
  if (allMetadataEqual) {
    setError(error,
             "The chapter vision model returned the same metadata for every "
             "chapter. Retry chapter analysis.");
    return false;
  }
  return true;
}

std::vector<std::int64_t> automaticChapterSampleTimes(std::int64_t durationUs) {
  if (durationUs < kMinimumAutomaticChapterVideoDurationUs) return {};
  const std::int64_t durationLimitedCount =
      durationUs / kMinimumAutomaticChapterDurationUs;
  const std::size_t count = static_cast<std::size_t>(std::clamp<std::int64_t>(
      durationLimitedCount,
      static_cast<std::int64_t>(kMinimumAutomaticChapterCount),
      static_cast<std::int64_t>(kMaximumAutomaticChapterCount)));
  const std::int64_t quotient = durationUs / static_cast<std::int64_t>(count);
  const std::int64_t remainder = durationUs % static_cast<std::int64_t>(count);
  std::vector<std::int64_t> times;
  times.reserve(count);
  for (std::size_t index = 0; index < count; ++index) {
    const std::int64_t signedIndex = static_cast<std::int64_t>(index);
    times.push_back(quotient * signedIndex +
                    (remainder * signedIndex) /
                        static_cast<std::int64_t>(count));
  }
  return times;
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

std::optional<std::int64_t> navigationTarget(
    const Snapshot& snapshot, std::int64_t positionUs,
    NavigationDirection direction) {
  const Chapter* current = chapterAt(snapshot, positionUs);
  if (!current) return std::nullopt;
  auto found = std::find_if(
      snapshot.chapters.begin(), snapshot.chapters.end(),
      [&](const Chapter& chapter) { return chapter.id == current->id; });
  if (found == snapshot.chapters.end()) return std::nullopt;
  if (direction == NavigationDirection::Previous) {
    return found == snapshot.chapters.begin()
               ? std::nullopt
               : std::optional<std::int64_t>((found - 1)->startUs);
  }
  ++found;
  return found == snapshot.chapters.end()
             ? std::nullopt
             : std::optional<std::int64_t>(found->startUs);
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
