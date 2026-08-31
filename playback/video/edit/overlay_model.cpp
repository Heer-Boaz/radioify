#include "playback/video/edit/overlay_model.h"

#include "playback/video/edit/command.h"
#include "playback/video/edit/scene_suggestions.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace playback_video_edit {
namespace {

std::string formatTimecode(int64_t timeUs, int64_t frameDurationUs,
                           bool compact = false) {
  if (timeUs < 0) return compact ? "--:--:--" : "--:--:--:--";
  const int64_t totalSeconds = timeUs / 1000000;
  const int64_t hours = totalSeconds / 3600;
  const int64_t minutes = (totalSeconds % 3600) / 60;
  const int64_t seconds = totalSeconds % 60;
  char buffer[64];
  if (frameDurationUs > 0) {
    const int64_t nominalFps = std::clamp<int64_t>(
        std::llround(1000000.0 / static_cast<double>(frameDurationUs)), 1, 999);
    const int64_t withinSecondUs = timeUs % 1000000;
    const int64_t frame = std::clamp<int64_t>(
        withinSecondUs * nominalFps / 1000000, 0, nominalFps - 1);
    if (compact && hours == 0) {
      std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld",
                    static_cast<long long>(minutes),
                    static_cast<long long>(seconds),
                    static_cast<long long>(frame));
    } else if (compact) {
      std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld:%02lld",
                    static_cast<long long>(hours),
                    static_cast<long long>(minutes),
                    static_cast<long long>(seconds),
                    static_cast<long long>(frame));
    } else {
      std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld:%02lld",
                    static_cast<long long>(hours),
                    static_cast<long long>(minutes),
                    static_cast<long long>(seconds),
                    static_cast<long long>(frame));
    }
    return std::string(buffer);
  }
  const int64_t milliseconds = (timeUs % 1000000) / 1000;
  if (compact && hours == 0) {
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld.%03lld",
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds),
                  static_cast<long long>(milliseconds));
  } else if (compact) {
    std::snprintf(buffer, sizeof(buffer), "%lld:%02lld:%02lld.%03lld",
                  static_cast<long long>(hours),
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds),
                  static_cast<long long>(milliseconds));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%02lld:%02lld:%02lld.%03lld",
                  static_cast<long long>(hours),
                  static_cast<long long>(minutes),
                  static_cast<long long>(seconds),
                  static_cast<long long>(milliseconds));
  }
  return std::string(buffer);
}

int64_t inclusiveOutDisplayUs(const EditSnapshot& edit) {
  if (!edit.outTimelineUs) return 0;
  // Edit ranges stay half-open so playback/export math remains unambiguous.
  // The editor-facing Out mark is inclusive, so label the last selected frame
  // instead of the exclusive boundary immediately after it.
  const int64_t lowerBound = edit.inTimelineUs.value_or(0);
  return std::clamp(edit.outFrameTimelineUs.value_or(*edit.outTimelineUs - 1),
                    lowerBound, *edit.outTimelineUs);
}

std::optional<int64_t> selectedDurationUs(const EditSnapshot& edit) {
  if (!edit.inTimelineUs || !edit.outTimelineUs ||
      edit.timelineDurationUs <= 0) {
    return std::nullopt;
  }
  const int64_t startUs = std::clamp(
      *edit.inTimelineUs, int64_t{0}, edit.timelineDurationUs);
  const int64_t endUs = std::clamp(
      *edit.outTimelineUs, int64_t{0}, edit.timelineDurationUs);
  if (endUs <= startUs) return std::nullopt;
  return endUs - startUs;
}

int timelineCell(int64_t timelineUs, int64_t timelineDurationUs, int width) {
  if (timelineDurationUs <= 0 || width <= 1) return 0;
  const long double ratio =
      static_cast<long double>(timelineUs) / timelineDurationUs;
  return std::clamp(static_cast<int>(std::llround(
                        ratio * static_cast<long double>(width - 1))),
                    0, width - 1);
}

std::string shortestFittingStatus(
    std::initializer_list<const char*> candidates, int width) {
  if (width <= 0) return {};
  for (const char* candidate : candidates) {
    const std::string status(candidate);
    if (static_cast<int>(status.size()) <= width) return status;
  }
  return {};
}

bool appendStatusPart(std::string* status, const std::string& part, int width) {
  if (!status || part.empty() || width <= 0) return false;
  const size_t separatorWidth = status->empty() ? 0 : 2;
  if (status->size() + separatorWidth + part.size() >
      static_cast<size_t>(width)) {
    return false;
  }
  if (separatorWidth != 0) *status += "  ";
  *status += part;
  return true;
}

SceneSuggestionCellKind suggestionCellKind(
    const SceneSuggestionSnapshot& suggestion) {
  if (suggestion.selected) return SceneSuggestionCellKind::Selected;
  switch (suggestion.kind) {
    case SceneSuggestionKind::Dialogue:
      return SceneSuggestionCellKind::Dialogue;
    case SceneSuggestionKind::Cutscene:
      return SceneSuggestionCellKind::Cutscene;
    case SceneSuggestionKind::MenuOrLoading:
      return SceneSuggestionCellKind::MenuOrLoading;
    case SceneSuggestionKind::Gameplay:
      return SceneSuggestionCellKind::None;
  }
  return SceneSuggestionCellKind::None;
}

int suggestionPriority(SceneSuggestionCellKind kind) {
  switch (kind) {
    case SceneSuggestionCellKind::Selected:
      return 4;
    case SceneSuggestionCellKind::Cutscene:
      return 3;
    case SceneSuggestionCellKind::Dialogue:
      return 2;
    case SceneSuggestionCellKind::MenuOrLoading:
      return 1;
    case SceneSuggestionCellKind::None:
      return 0;
  }
  return 0;
}

const char* shortSuggestionLabel(SceneSuggestionKind kind) {
  switch (kind) {
    case SceneSuggestionKind::Gameplay:
      return "GAMEPLAY";
    case SceneSuggestionKind::Dialogue:
      return "DIALOGUE";
    case SceneSuggestionKind::Cutscene:
      return "CUTSCENE";
    case SceneSuggestionKind::MenuOrLoading:
      return "MENU/LOAD";
  }
  return "SEGMENT";
}

}  // namespace

std::string retainedProgramBadge(const EditSnapshot& edit) {
  if (edit.active || !edit.hasEdits) return {};
  return edit.hasUnexportedChanges ? "[EDITED*]" : "[EDITED]";
}

bool needsOverlayPresentation(const EditSnapshot& edit,
                              const ExportProgress& editExport,
                              Prompt prompt) {
  return edit.active || prompt != Prompt::None || editExport.visible() ||
         edit.sceneAnalysisStatus == SceneAnalysisStatus::Running ||
         edit.sceneAnalysisStatus == SceneAnalysisStatus::Failed;
}

OverlayModel buildOverlayModel(const EditSnapshot& edit,
                               const ExportProgress* editExport,
                               Prompt prompt, int width,
                               double timelineProgress) {
  OverlayModel model;
  const ExportProgress idleExport;
  const ExportProgress& exportProgress =
      editExport ? *editExport : idleExport;
  const bool exportRunning = exportProgress.running();
  const bool exportFailed = exportProgress.failed();
  const bool analysisRunning =
      edit.sceneAnalysisStatus == SceneAnalysisStatus::Running;
  const bool analysisFailed =
      edit.sceneAnalysisStatus == SceneAnalysisStatus::Failed;
  if (width <= 0 ||
      !needsOverlayPresentation(edit, exportProgress, prompt)) {
    return model;
  }

  // Timeline geometry is independent from the status shown above it.  Keep
  // the current program projection behind modal editor prompts so opening a
  // confirmation never replaces the editor timeline with the playback bar.
  if (edit.active && edit.timelineDurationUs > 0) {
    const bool hasSelectedRange =
        edit.inTimelineUs.has_value() && edit.outTimelineUs.has_value();
    const int64_t selectionStartUs = edit.inTimelineUs.value_or(0);
    const int64_t selectionEndUs = edit.outTimelineUs.value_or(0);
    model.cells.reserve(static_cast<size_t>(width));
    size_t clipIndex = 0;
    for (int cell = 0; cell < width; ++cell) {
      const long double ratio =
          static_cast<long double>(2LL * cell + 1) /
          static_cast<long double>(2LL * width);
      const int64_t timelineUs = static_cast<int64_t>(
          ratio * static_cast<long double>(edit.timelineDurationUs));
      while (clipIndex + 1 < edit.clips.size() &&
             timelineUs >= edit.clips[clipIndex + 1].timelineStartUs) {
        ++clipIndex;
      }
      const bool selected =
          hasSelectedRange && timelineUs >= selectionStartUs &&
          timelineUs < selectionEndUs;
      model.cells.push_back(
          selected ? TimelineCellKind::Selected
                   : (clipIndex % 2 == 0 ? TimelineCellKind::Kept
                                         : TimelineCellKind::KeptAlternate));
    }
    model.playheadCell = edit.playheadTimelineUs
                             ? timelineCell(*edit.playheadTimelineUs,
                                            edit.timelineDurationUs, width)
                             : std::clamp(static_cast<int>(std::llround(
                                              std::clamp(timelineProgress, 0.0,
                                                         1.0) *
                                              static_cast<double>(width - 1))),
                                          0, width - 1);
    if (edit.inTimelineUs) {
      model.inCell = timelineCell(*edit.inTimelineUs,
                                  edit.timelineDurationUs, width);
    }
    if (edit.outTimelineUs) {
      model.outCell = timelineCell(*edit.outTimelineUs,
                                   edit.timelineDurationUs, width);
    }
    model.cutCells.reserve(edit.cuts.size());
    model.smoothCutCells.reserve(edit.cuts.size());
    for (const EditCutSnapshot& cut : edit.cuts) {
      const int cutCell =
          timelineCell(cut.timelineUs, edit.timelineDurationUs, width);
      std::vector<int>& destination =
          cut.transition.kind == CutTransitionKind::MotionSmooth
              ? model.smoothCutCells
              : model.cutCells;
      if (destination.empty() || destination.back() != cutCell) {
        destination.push_back(cutCell);
      }
    }

    if (!edit.suggestionReview.suggestions.empty()) {
      model.sceneSuggestionCells.assign(
          static_cast<size_t>(width), SceneSuggestionCellKind::None);
      for (const SceneSuggestionSnapshot& suggestion :
           edit.suggestionReview.suggestions) {
        const SceneSuggestionCellKind kind = suggestionCellKind(suggestion);
        for (const SceneSuggestionSpanSnapshot& span : suggestion.spans) {
          const int startCell = timelineCell(
              span.timelineStartUs, edit.timelineDurationUs, width);
          const int endCell = timelineCell(
              std::max(span.timelineStartUs, span.timelineEndUs - 1),
              edit.timelineDurationUs, width);
          if (model.sceneSuggestionBoundaryCells.empty() ||
              model.sceneSuggestionBoundaryCells.back() != startCell) {
            model.sceneSuggestionBoundaryCells.push_back(startCell);
          }
          if (kind == SceneSuggestionCellKind::None) continue;
          for (int cell = startCell; cell <= endCell; ++cell) {
            auto& destination = model.sceneSuggestionCells[
                static_cast<size_t>(cell)];
            if (suggestionPriority(kind) >
                suggestionPriority(destination)) {
              destination = kind;
            }
          }
        }
      }
      std::sort(model.sceneSuggestionBoundaryCells.begin(),
                model.sceneSuggestionBoundaryCells.end());
      model.sceneSuggestionBoundaryCells.erase(
          std::unique(model.sceneSuggestionBoundaryCells.begin(),
                      model.sceneSuggestionBoundaryCells.end()),
          model.sceneSuggestionBoundaryCells.end());
    }
  }

  if (prompt == Prompt::LeaveEditMode) {
    model.status = shortestFittingStatus(
        {"LEAVE EDIT MODE?", "LEAVE EDIT?", "LEAVE?"}, width);
    return model;
  }
  if (prompt == Prompt::DiscardEdits) {
    model.status = shortestFittingStatus(
        {"DISCARD ALL EDITS?", "DISCARD EDITS?", "DISCARD?"}, width);
    return model;
  }
  if (prompt == Prompt::LeavePlayback) {
    const ExitExportAction exportAction = exitExportAction({
        edit.hasUnexportedChanges,
        exportRunning,
        editExport && editExport->targetsCurrentRevision,
    });
    if (exportFailed) {
      model.status =
          shortestFittingStatus({"EXPORT FAILED", "FAILED", "!"}, width);
    } else if (exportAction == ExitExportAction::CancelBlockingExport) {
      model.status = shortestFittingStatus(
          {"UNEXPORTED EDITS  EXPORT BUSY", "UNEXPORTED  BUSY",
           "UNEXPORTED", "EDIT*", "*"},
          width);
    } else if (exportRunning) {
      model.status = shortestFittingStatus(
          {"EXPORT RUNNING", "EXPORTING", "EXPORT"}, width);
    } else if (edit.hasUnexportedChanges) {
      model.status = shortestFittingStatus(
          {"UNEXPORTED EDITS", "UNEXPORTED", "EDIT*", "*"}, width);
    } else {
      model.status =
          shortestFittingStatus({"LEAVE PLAYBACK?", "LEAVE?", "EXIT?"},
                                width);
    }
    return model;
  }

  if (exportFailed) {
    appendStatusPart(
        &model.status,
        shortestFittingStatus({"EXPORT FAILED", "FAILED", "!"}, width),
        width);
  }

  if (analysisFailed) {
    appendStatusPart(
        &model.status,
        shortestFittingStatus(
            {"SEGMENT DETECTION FAILED", "DETECTION FAILED", "FAILED"},
            width),
        width);
  } else if (analysisRunning) {
    const int percentage = static_cast<int>(std::lround(
        std::clamp(edit.sceneAnalysisProgress, 0.0, 1.0) * 100.0));
    if (!appendStatusPart(&model.status,
                          "SEGMENTS " + std::to_string(percentage) + "%",
                          width)) {
      appendStatusPart(&model.status,
                       shortestFittingStatus(
                           {"DETECTING", "SCANNING"}, width),
                       width);
    }
  } else if (edit.active && edit.suggestionReview.visible &&
             !edit.suggestionReview.suggestions.empty()) {
    const auto selected = std::find_if(
        edit.suggestionReview.suggestions.begin(),
        edit.suggestionReview.suggestions.end(),
        [](const SceneSuggestionSnapshot& suggestion) {
          return suggestion.selected;
        });
    if (selected != edit.suggestionReview.suggestions.end()) {
      const std::string ordinal =
          edit.suggestionReview.selectedOrdinal
              ? std::to_string(*edit.suggestionReview.selectedOrdinal) +
                    "/" +
                    std::to_string(edit.suggestionReview.filteredCount) + " "
              : std::string{};
      const std::string kind = shortSuggestionLabel(selected->kind);
      const std::string strength =
          sceneSuggestionStrengthLabel(selected->confidence);
      const std::string full =
          ordinal + kind + " " + strength + " " +
          formatTimecode(selected->source.startUs,
                         edit.timecodeFrameDurationUs, true) +
          "-" +
          formatTimecode(selected->source.endUs,
                         edit.timecodeFrameDurationUs, true);
      if (!appendStatusPart(&model.status, full, width) &&
          !appendStatusPart(&model.status,
                            ordinal + kind + " " + strength, width)) {
        appendStatusPart(&model.status, ordinal + kind, width);
      }
    } else {
      appendStatusPart(&model.status,
                       std::to_string(
                           edit.suggestionReview.suggestions.size()) +
                           " SUGGESTIONS",
                       width);
    }
  } else if (edit.active && edit.suggestionReview.visible &&
             edit.sceneAnalysisStatus == SceneAnalysisStatus::Ready) {
    appendStatusPart(
        &model.status,
        std::string("NO ") +
            sceneSuggestionFilterLabel(edit.suggestionReview.filter) +
            " SUGGESTIONS",
        width);
  }

  if (edit.active && edit.suggestionReview.visible &&
      edit.sceneAnalysisStatus == SceneAnalysisStatus::Ready) {
    appendStatusPart(&model.status,
                     edit.sceneAnalysisUsedTranscript
                         ? "VIDEO + TRANSCRIPT"
                         : "VIDEO ONLY",
                     width);
  }

  if (edit.active) {
    appendStatusPart(
        &model.status,
        edit.hasUnexportedChanges
            ? shortestFittingStatus({"EDITING*", "EDIT*", "*"}, width)
            : shortestFittingStatus({"EDITING", "EDIT"}, width),
        width);
    if (const auto durationUs = selectedDurationUs(edit)) {
      appendStatusPart(
          &model.status,
          "SELECTED " + formatTimecode(*durationUs,
                                         edit.timecodeFrameDurationUs, true),
          width);
    } else if (edit.inTimelineUs) {
      appendStatusPart(
          &model.status,
          "START " + formatTimecode(*edit.inTimelineUs,
                                      edit.timecodeFrameDurationUs, true),
          width);
    } else if (edit.outTimelineUs) {
      appendStatusPart(
          &model.status,
          "END " + formatTimecode(inclusiveOutDisplayUs(edit),
                                    edit.timecodeFrameDurationUs, true),
          width);
    }
    if (!edit.cuts.empty()) {
      appendStatusPart(&model.status,
                       std::to_string(edit.cuts.size()) + " REMOVED", width);
    }
  }
  if (exportRunning) {
    const int percentage = static_cast<int>(
        std::lround(std::clamp(editExport->fraction, 0.0, 1.0) * 100.0));
    const bool targetsCurrent = editExport->targetsCurrentRevision;
    const std::string progressLabel =
        std::string(targetsCurrent ? "EXPORT " : "OLDER EXPORT ") +
        std::to_string(percentage) + "%";
    if (!appendStatusPart(&model.status,
                          progressLabel, width)) {
      appendStatusPart(
          &model.status,
          targetsCurrent
              ? shortestFittingStatus({"EXPORTING", "EXPORT", "EXP"},
                                      width)
              : shortestFittingStatus(
                    {"OLDER EXPORT", "OLD EXPORT", "EXPORT"}, width),
          width);
    }
  }
  if (edit.active && edit.playheadTimelineUs) {
    appendStatusPart(
        &model.status,
        "TC " + formatTimecode(*edit.playheadTimelineUs,
                                edit.timecodeFrameDurationUs) +
            " / " +
            formatTimecode(edit.timelineDurationUs,
                           edit.timecodeFrameDurationUs),
        width);
  }
  return model;
}

}  // namespace playback_video_edit
