#pragma once

#include <cstdint>

namespace playback_video_edit {

// Semantic edit commands shared by shortcuts, controls, and context menus.
// Presentation identifiers never cross this boundary.
enum class Command : uint8_t {
  Open,
  RequestClose,
  ConfirmClose,
  CancelClose,
  MarkIn,
  ClearIn,
  MarkOut,
  ClearOut,
  ClearInAndOut,
  RippleDelete,
  Trim,
  Undo,
  Redo,
  Reset,
  Export,
  Discard,
};

}  // namespace playback_video_edit
