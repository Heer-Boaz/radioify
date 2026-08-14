#pragma once

#include <cstdint>

namespace playback_video_edit {

// Semantic edit commands shared by shortcuts, controls, and context menus.
// Presentation identifiers never cross this boundary.
enum class Command : uint8_t {
  Open,
  Finish,
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

// Finishing is a workflow boundary, not a synonym for closing the tools.
// Radioify has no persistent project file, so an unexported revision must be
// made durable before the editor can truthfully report that it is done.
// Selection is intentionally absent: unapplied In/Out marks are transient UI
// state, not document decisions that finishing may interpret or persist.
enum class FinishAction : uint8_t {
  Close,
  StartExport,
  WaitForExport,
};

struct FinishContext {
  bool hasUnexportedChanges = false;
  bool exportRunning = false;
  // True only when an in-flight or successful output contains the exact
  // current edit revision. Failed and cancelled jobs never satisfy this.
  bool exportCoversCurrentRevision = false;
};

constexpr FinishAction finishAction(const FinishContext& context) {
  if (!context.hasUnexportedChanges) return FinishAction::Close;
  if (context.exportCoversCurrentRevision) return FinishAction::Close;
  return context.exportRunning ? FinishAction::WaitForExport
                               : FinishAction::StartExport;
}

}  // namespace playback_video_edit
