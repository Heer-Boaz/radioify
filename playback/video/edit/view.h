#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "playback/video/edit/decision_list.h"

namespace playback_video_edit {

enum class EditBoundary : uint8_t {
  In,
  Out,
};

// Explicit modal state shared by input and both renderers. A prompt is an
// application decision, never something a renderer infers from dirty state.
enum class Prompt : uint8_t {
  None,
  LeaveEditMode,
  DiscardEdits,
  LeavePlayback,
  RestartAnalysis,
};

struct EditClipSnapshot {
  SourceRange source;
  int64_t timelineStartUs = 0;

  int64_t timelineEndUs() const {
    return timelineStartUs + source.durationUs();
  }
};

struct EditCutSnapshot {
  int64_t timelineUs = 0;
  CutTransition transition;
};

enum class SceneAnalysisStatus : uint8_t {
  Idle,
  Running,
  Pausing,
  Ready,
  Failed,
  Cancelled,
};

enum class SceneSuggestionKind : uint8_t {
  Keep,
  Shorten,
  Review,
};

enum class SceneSuggestionFilter : uint8_t {
  All,
  Keep,
  Shorten,
  Review,
};

struct SceneSuggestionSpanSnapshot {
  int64_t timelineStartUs = 0;
  int64_t timelineEndUs = 0;
};

struct SceneSuggestionSnapshot {
  uint64_t id = 0;
  SourceRange source;
  SceneSuggestionKind kind = SceneSuggestionKind::Review;
  std::string reason;
  bool selected = false;
  std::vector<SceneSuggestionSpanSnapshot> spans;
};

struct SceneSuggestionReviewSnapshot {
  bool visible = false;
  SceneSuggestionFilter filter = SceneSuggestionFilter::All;
  size_t totalCount = 0;
  size_t filteredCount = 0;
  // One-based position inside the active filter, for presentation as N / M.
  std::optional<size_t> selectedOrdinal;
  bool canUndoHide = false;
  std::vector<SceneSuggestionSnapshot> suggestions;
  std::optional<uint64_t> selectedId;
  // -1 follows the selected item; an explicit offset permits independent scrolling.
  int scrollOffset = -1;
  std::array<int64_t, 3> durationByKindUs{};
  bool previewing = false;
};

// Immutable value state consumed by both ASCII and framebuffer renderers.
// All fields carrying "timeline" time use the edited program timeline. Source
// time is kept explicit and is never used as the seek-bar coordinate system.
struct EditSnapshot {
  bool active = false;
  // Independent axes: edit decisions can remain after their latest output was
  // rendered, while only newer decisions count as unexported.
  bool hasEdits = false;
  bool hasUnexportedChanges = false;
  bool canRippleDelete = false;
  bool canTrim = false;
  bool canUndo = false;
  bool canRedo = false;
  bool canToggleSmoothCut = false;
  int64_t sourceDurationUs = 0;
  int64_t timelineDurationUs = 0;
  // Stable sequence timebase for display. This is deliberately not the
  // duration of whichever VFR frame happens to be under the playhead.
  int64_t timecodeFrameDurationUs = 0;
  std::vector<SourceRange> keptRanges;
  std::vector<EditClipSnapshot> clips;
  std::vector<EditCutSnapshot> cuts;
  // Scene analysis is an immutable, source-coordinate suggestion layer. It is
  // projected through the current EDL for presentation but never participates
  // in document history until the user invokes an existing edit command.
  SceneAnalysisStatus sceneAnalysisStatus = SceneAnalysisStatus::Idle;
  double sceneAnalysisProgress = 0.0;
  std::string sceneAnalysisPhase;
  std::string sceneAnalysisError;
  SceneSuggestionReviewSnapshot suggestionReview;
  std::optional<CutTransition> selectedCutTransition;
  std::optional<int64_t> inSourceUs;
  std::optional<int64_t> outSourceUs;
  std::optional<int64_t> inTimelineUs;
  std::optional<int64_t> outTimelineUs;
  // Exact start of the frame selected as Out. The selection boundary above
  // remains exclusive; keeping both prevents later playhead movement (or VFR
  // cadence) from changing the displayed inclusive Out timecode.
  std::optional<int64_t> outFrameTimelineUs;
  std::optional<int64_t> playheadTimelineUs;
};

// Presentation-only export state. Renderers must not depend on worker,
// filesystem, encoder, or error-reporting types. A failed status represents
// the current document revision only; stale job results are filtered by the
// workspace before crossing this boundary.
enum class ExportStatus : uint8_t {
  Idle,
  Running,
  Failed,
};

struct ExportProgress {
  ExportStatus status = ExportStatus::Idle;
  double fraction = 0.0;
  // A running encoder owns an immutable decision-list snapshot. This flag is
  // true only when that snapshot is the document revision currently shown.
  bool targetsCurrentRevision = false;

  bool running() const { return status == ExportStatus::Running; }
  bool failed() const { return status == ExportStatus::Failed; }
  bool visible() const { return status != ExportStatus::Idle; }
};

}  // namespace playback_video_edit
