#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace playback_video_chapters {

enum class AnalysisState : std::uint8_t {
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
                       const std::vector<Chapter>& chapters,
                       std::string* error = nullptr);

const Chapter* chapterAt(const Snapshot& snapshot, std::int64_t positionUs);

enum class NavigationDirection : std::uint8_t {
  Previous,
  Next,
};

// Resolves chapter navigation against the immutable partition. Returning no
// target is intentional at either edge and while analysis is not ready; input
// routing must never fall back to playlist navigation for the same gesture.
std::optional<std::int64_t> navigationTarget(
    const Snapshot& snapshot, std::int64_t positionUs,
    NavigationDirection direction);

// Projects interior chapter boundaries into renderer addressable units.
// Multiple boundaries that collapse onto one unit are reported once in
// boundaryCells and separately in collisionCells so a renderer can use a
// stronger marker without changing chapter semantics.
struct MarkerProjection {
  std::vector<int> boundaryCells;
  std::vector<int> collisionCells;
};

MarkerProjection projectMarkers(const Snapshot& snapshot, int units);

const char* analysisStateLabel(AnalysisState state);

}  // namespace playback_video_chapters
