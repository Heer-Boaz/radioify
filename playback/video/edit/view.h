#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace playback_video_edit {

// Source-space, half-open media interval. Edit decisions always reference the
// immutable source; output time is derived by concatenating these intervals.
struct SourceRange {
  int64_t startUs = 0;
  int64_t endUs = 0;

  int64_t durationUs() const { return endUs - startUs; }
};

inline bool operator==(const SourceRange& lhs, const SourceRange& rhs) {
  return lhs.startUs == rhs.startUs && lhs.endUs == rhs.endUs;
}

inline bool operator!=(const SourceRange& lhs, const SourceRange& rhs) {
  return !(lhs == rhs);
}

// Immutable value state consumed by both ASCII and framebuffer renderers.
struct EditSnapshot {
  bool active = false;
  int64_t sourceDurationUs = 0;
  int64_t outputDurationUs = 0;
  std::vector<SourceRange> keptRanges;
  std::optional<int64_t> inUs;
  std::optional<int64_t> outUs;
};

// Presentation-only export state. Renderers must not depend on worker,
// filesystem, encoder, or error-reporting types.
struct ExportProgress {
  bool active = false;
  double fraction = 0.0;

  bool running() const { return active; }
};

}  // namespace playback_video_edit
