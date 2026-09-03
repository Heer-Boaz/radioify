#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playback_video_chapters {

inline constexpr std::size_t kMinimumAutomaticChapterCount = 1;
// Keep a defensive storage bound without imposing an editorial "handful of
// chapters" policy. Professional chapter formats are duration-driven; the
// ten-second minimum and sampler-owned anchors provide the real density limit.
inline constexpr std::size_t kMaximumAutomaticChapterCount = 64;
// The complete-timeline scan is reduced to at most one hundred timestamped
// temporal windows. Each window may contain several chronological frames;
// evidence density remains independent of the published chapter count.
inline constexpr std::size_t kMaximumAutomaticEvidenceSampleCount = 100;
inline constexpr std::size_t kMinimumAutomaticEvidenceSampleCount = 3;
inline constexpr std::size_t kMaximumAutomaticEvidenceFrameCount =
    kMaximumAutomaticEvidenceSampleCount * 6 + 1;
inline constexpr std::int64_t kMinimumAutomaticChapterDurationUs = 10'000'000;
inline constexpr std::int64_t kMinimumAutomaticChapterVideoDurationUs =
    static_cast<std::int64_t>(kMinimumAutomaticEvidenceSampleCount) *
    kMinimumAutomaticChapterDurationUs;
// Chapter-Llama is evaluated on 30-60 minute sources. Keep the automatic
// runtime feature inside that published envelope until a separately validated
// hierarchical boundary planner owns longer media.
inline constexpr std::int64_t kMaximumAutomaticChapterVideoDurationUs =
    60LL * 60LL * 1'000'000LL;
inline constexpr std::size_t kMaximumAutomaticCaptionBytes = 768;
inline constexpr std::size_t kMaximumAutomaticTitleBytes = 160;
inline constexpr std::size_t kMaximumAutomaticSummaryBytes = 600;
inline constexpr std::size_t kMaximumAutomaticOverviewBytes = 1200;

enum class AnalysisState : std::uint8_t {
  Disabled,
  CheckingSupport,
  SetupRequired,
  Installing,
  WaitingForPlayback,
  Analyzing,
  Ready,
  Unsupported,
  Failed,
};

struct Chapter {
  std::uint64_t id = 0;
  std::int64_t startUs = 0;
  std::int64_t endUs = 0;
  std::string title;
  std::string summary;
};

// Immutable session-facing projection of automatic chapter analysis. The
// worker publishes complete revisions only; renderers never observe a
// partially constructed chapter partition.
struct Snapshot {
  AnalysisState state = AnalysisState::CheckingSupport;
  std::int64_t durationUs = 0;
  std::optional<double> progress;
  std::string phase;
  std::string detail;
  std::string warning;
  std::string overview;
  std::vector<Chapter> chapters;
  std::uint64_t revision = 0;

  bool running() const {
    return state == AnalysisState::Installing ||
           state == AnalysisState::WaitingForPlayback ||
           state == AnalysisState::Analyzing;
  }
  bool ready() const {
    return state == AnalysisState::Ready && !chapters.empty();
  }
};

// Generated chapters form one exact, ordered partition of the source. This is
// the same complete-timeline contract used by established chaptered players:
// the first chapter begins at zero and the final chapter ends at duration.
bool validatePartition(std::int64_t durationUs,
                       const std::vector<Chapter> &chapters,
                       std::string *error = nullptr);

// Product policy for generated timeline ranges. Homogeneous videos may have a
// single chapter; multiple markers are only published when semantic evidence
// supports change points. Every range remains at least ten seconds.
bool validateAutomaticPartition(std::int64_t durationUs,
                                const std::vector<Chapter> &chapters,
                                std::string *error = nullptr);

// Validates the complete publishable automatic-analysis artifact. Unlike the
// timeline-only partition contract, this also rejects missing or visibly
// truncated prose and a fully collapsed response that repeats the same title
// and summary for every section. Individual titles or summaries may repeat;
// real chapter formats do not require artificial uniqueness.
bool validateAutomaticAnalysis(std::int64_t durationUs,
                               std::string_view overview,
                               const std::vector<Chapter> &chapters,
                               std::string *error = nullptr);

const Chapter *chapterAt(const Snapshot &snapshot, std::int64_t positionUs);

enum class NavigationDirection : std::uint8_t {
  Previous,
  Next,
};

// Resolves chapter navigation against the immutable partition. Returning no
// target is intentional at either edge and while analysis is not ready; input
// routing must never fall back to playlist navigation for the same gesture.
std::optional<std::int64_t> navigationTarget(const Snapshot &snapshot,
                                             std::int64_t positionUs,
                                             NavigationDirection direction);

// Projects interior chapter boundaries into renderer addressable units.
// Multiple boundaries that collapse onto one unit are reported once in
// boundaryCells and separately in collisionCells so a renderer can use a
// stronger marker without changing chapter semantics.
struct MarkerProjection {
  std::vector<int> boundaryCells;
  std::vector<int> collisionCells;
};

MarkerProjection projectMarkers(const Snapshot &snapshot, int units);

const char *analysisStateLabel(AnalysisState state);

} // namespace playback_video_chapters
