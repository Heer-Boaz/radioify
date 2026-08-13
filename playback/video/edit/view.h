#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "playback/video/sequence.h"

namespace playback_video_edit {

using playback_video_sequence::SourceRange;

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
};

struct EditClipSnapshot {
  SourceRange source;
  int64_t timelineStartUs = 0;

  int64_t timelineEndUs() const {
    return timelineStartUs + source.durationUs();
  }
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
  int64_t sourceDurationUs = 0;
  int64_t timelineDurationUs = 0;
  int64_t frameDurationUs = 0;
  std::vector<SourceRange> keptRanges;
  std::vector<EditClipSnapshot> clips;
  std::optional<int64_t> inSourceUs;
  std::optional<int64_t> outSourceUs;
  std::optional<int64_t> inTimelineUs;
  std::optional<int64_t> outTimelineUs;
  std::optional<int64_t> playheadTimelineUs;
};

// Presentation-only export state. Renderers must not depend on worker,
// filesystem, encoder, or error-reporting types.
struct ExportProgress {
  bool active = false;
  double fraction = 0.0;

  bool running() const { return active; }
};

}  // namespace playback_video_edit
