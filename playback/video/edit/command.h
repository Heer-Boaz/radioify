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
  ToggleIn,
  ClearIn,
  MarkOut,
  ToggleOut,
  ClearOut,
  ClearInAndOut,
  StartSceneAnalysis,
  RestartSceneAnalysis,
  CancelSceneAnalysis,
  ToggleSceneSuggestions,
  CloseSceneSuggestions,
  PreviewSceneSuggestion,
  StopScenePreview,
  CycleSceneSuggestionFilter,
  PreviousSceneSuggestion,
  NextSceneSuggestion,
  SelectSceneSuggestion,
  DismissSceneSuggestion,
  UndoDismissSceneSuggestion,
  RippleDelete,
  Trim,
  ToggleSmoothCut,
  Undo,
  Redo,
  Reset,
  StartExport,
  CancelExport,
};

// Leaving playback and leaving the edit tools are different workflow
// boundaries. Only the playback-exit flow decides how an unexported document
// or a background export must be resolved.
enum class ExitExportAction : uint8_t {
  None,
  ExportCurrent,
  WaitForExport,
  CancelBlockingExport,
};

struct ExitContext {
  bool hasUnexportedChanges = false;
  bool exportRunning = false;
  bool exportTargetsCurrentRevision = false;
};

constexpr ExitExportAction exitExportAction(const ExitContext& context) {
  if (!context.exportRunning) {
    return context.hasUnexportedChanges ? ExitExportAction::ExportCurrent
                                        : ExitExportAction::None;
  }
  if (context.hasUnexportedChanges &&
      !context.exportTargetsCurrentRevision) {
    return ExitExportAction::CancelBlockingExport;
  }
  return ExitExportAction::WaitForExport;
}

}  // namespace playback_video_edit
