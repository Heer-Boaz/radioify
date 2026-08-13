#pragma once

#include <cstdint>

namespace playback_video_edit {

// Semantic edit commands shared by shortcuts, controls, and context menus.
// Presentation identifiers never cross this boundary.
enum class Command : uint8_t {
  Open,
  RequestClose,
  RequestDiscard,
  ConfirmPrompt,
  CancelPrompt,
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
};

}  // namespace playback_video_edit
