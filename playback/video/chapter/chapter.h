#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace playback_video_chapters {

inline constexpr std::size_t kMinimumAutomaticChapterCount = 1;
// This is a storage/admission limit, not an editorial chapter-count policy.
// It matches Chapter-Llama's published maximum caption-selection cardinality.
inline constexpr std::size_t kMaximumAutomaticChapterCount = 100;
inline constexpr std::int64_t kMinimumAutomaticChapterVideoDurationUs =
    30'000'000;
// Radioify's first runtime contract admits 30-second through 60-minute videos.
// The upper bound avoids inventing a local replacement for Chapter-Llama's
// separately published iterative long-context procedure.
inline constexpr std::int64_t kMaximumAutomaticChapterVideoDurationUs =
    60LL * 60LL * 1'000'000LL;
// The reference caption extractor permits 1024 generated tokens. Keep the
// per-frame storage bound large enough for that native response while the
// aggregate planner-context budget remains the authoritative admission limit.
inline constexpr std::size_t kMaximumAutomaticCaptionBytes = 4096;
inline constexpr std::size_t kMaximumAutomaticTitleBytes = 160;

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
  std::vector<Chapter> chapters;
  std::uint64_t revision = 0;

  bool running() const {
    return state == AnalysisState::CheckingSupport ||
           state == AnalysisState::Installing ||
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
// supports change points. Radioify does not impose an invented minimum chapter
// length on the model output.
bool validateAutomaticPartition(std::int64_t durationUs,
                                const std::vector<Chapter> &chapters,
                                std::string *error = nullptr);

// Validates the complete publishable automatic chapter artifact.
bool validateAutomaticAnalysis(std::int64_t durationUs,
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

// Presentation-time projection for a source-coordinate chapter partition.
// Segments must form one contiguous presentation timeline but may skip source
// ranges. A cut crossing chapter identity produces one boundary at the join.
struct MarkerTimelineSegment {
  std::int64_t sourceStartUs = 0;
  std::int64_t sourceEndUs = 0;
  std::int64_t presentationStartUs = 0;
};

// Materializes the single chapter view consumed by markers, hover metadata,
// the overview and navigation after an edit decision list is applied. Source
// chapters which are fully removed disappear; adjacent retained fragments of
// the same chapter remain one semantic chapter.
std::optional<Snapshot> projectToPresentationTimeline(
    const Snapshot &snapshot, std::int64_t presentationDurationUs,
    const std::vector<MarkerTimelineSegment> &segments);

const char *analysisStateLabel(AnalysisState state);

} // namespace playback_video_chapters
