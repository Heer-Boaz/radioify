#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "playback/video/sequence.h"

namespace playback_video_edit {

using playback_video_sequence::SourceRange;

// Immutable value state consumed by both ASCII and framebuffer renderers.
struct EditSnapshot {
  bool active = false;
  int64_t sourceDurationUs = 0;
  int64_t outputDurationUs = 0;
  std::vector<SourceRange> keptRanges;
  std::optional<int64_t> inUs;
  std::optional<int64_t> outUs;
  std::optional<int64_t> playheadSourceUs;
};

// Presentation-only export state. Renderers must not depend on worker,
// filesystem, encoder, or error-reporting types.
struct ExportProgress {
  bool active = false;
  double fraction = 0.0;

  bool running() const { return active; }
};

}  // namespace playback_video_edit
