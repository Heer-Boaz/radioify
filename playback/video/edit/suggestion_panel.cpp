#include "playback/video/edit/suggestion_panel.h"

#include <algorithm>
#include <cstdio>

#include "core/unicode_display_width.h"
#include "playback/video/edit/scene_suggestions.h"

namespace playback_video_edit {
namespace {

std::string duration(int64_t timeUs) {
  const auto seconds = std::max<int64_t>(0, timeUs) / 1'000'000;
  char text[48];
  if (seconds >= 3600)
    std::snprintf(text, sizeof(text), "%lld:%02lld:%02lld", seconds / 3600,
                    seconds / 60 % 60, seconds % 60);
  else
    std::snprintf(text, sizeof(text), "%lld:%02lld", seconds / 60, seconds % 60);
  return text;
}

}  // namespace

SuggestionPresentation buildSuggestionPresentation(const EditSnapshot& edit) {
  SuggestionPresentation presentation;
  if (!edit.active) return presentation;
  const auto& review = edit.suggestionReview;
  const auto action = [&](Command command, std::string label,
                          bool enabled = true, bool active = false) {
    presentation.actions.push_back({command, std::move(label), enabled, active});
  };
  const auto append = [&](std::string text, bool accent = false) {
    presentation.introduction.push_back({std::move(text), accent});
  };
  action(Command::ToggleSceneSuggestions, kSuggestionPanelTitle, true, review.visible);
  if (!review.visible) return presentation;

  switch (edit.sceneAnalysisStatus) {
    case SceneAnalysisStatus::Idle:
      action(Command::StartSceneAnalysis, "Start analysis");
      append("Find moments worth keeping and stretches you could shorten.", true);
      append("Experimental AI suggestions. Check the footage before cutting.");
      append("Analyses the original video (" + duration(edit.sourceDurationUs) + "). Your edits stay unchanged.");
      append("Visual-only, 2 frames/second. Brief moments and audio can be missed.");
      append("Runs locally on NVIDIA. First use downloads a model (~6 GB); waits for free GPU memory.");
      append("Start analysis to begin. Saved results or progress are reused.");
      break;
    case SceneAnalysisStatus::Running:
      action(Command::CancelSceneAnalysis, "Pause analysis");
      append("Finding edit suggestions", true);
      append(edit.sceneAnalysisPhase);
      if (edit.sceneAnalysisProgress > 0.0)
        append("Progress: " + std::to_string(static_cast<int>(
            std::clamp(edit.sceneAnalysisProgress, 0.0, 1.0) * 100)) + "%");
      append("You can keep editing. Closing this panel keeps analysis running; Pause saves completed work.");
      append("Suggestions appear when the analysis finishes.");
      break;
    case SceneAnalysisStatus::Pausing:
      action(Command::CancelSceneAnalysis, "Pause analysis", false);
      append("Pausing analysis...", true);
      append("Waiting for the worker to stop. Completed work is saved; you can resume once it has stopped.");
      break;
    case SceneAnalysisStatus::Cancelled:
      action(Command::StartSceneAnalysis, "Resume analysis");
      append("Analysis paused", true);
      append("Resume to continue from saved progress. Your edits are unchanged.");
      break;
    case SceneAnalysisStatus::Failed:
      action(Command::StartSceneAnalysis, "Retry analysis");
      append("Analysis stopped", true);
      append(edit.sceneAnalysisError);
      append("Retry continues from saved progress. Your edits are unchanged.");
      break;
    case SceneAnalysisStatus::Ready: {
      action(Command::CycleSceneSuggestionFilter,
             std::string("Filter: ") + sceneSuggestionFilterLabel(review.filter),
             true, review.filter != SceneSuggestionFilter::All);
      const bool hasSelection = review.selectedId.has_value();
      action(Command::PreviousSceneSuggestion, "Previous", hasSelection);
      action(Command::NextSceneSuggestion, "Next", hasSelection);
      if (review.previewing)
        action(Command::StopScenePreview, "Stop preview", true, true);
      else
        action(Command::PreviewSceneSuggestion, "Preview", hasSelection);
      action(Command::SelectSceneSuggestion, "Select range", hasSelection);
      action(Command::DismissSceneSuggestion, "Hide suggestion", hasSelection);
      if (review.canUndoHide)
        action(Command::UndoDismissSceneSuggestion, "Undo hide");
      action(Command::RestartSceneAnalysis, "Analyse again...");
      append("Suggested: Keep " + duration(review.durationByKindUs[0]) +
             " | Shorten " + duration(review.durationByKindUs[1]) +
             " | Review " + duration(review.durationByKindUs[2]));
      append("Experimental AI suggestions. Check the footage before cutting.");
      append("Preview plays a suggestion and pauses at its end. Select range sets marks; only Remove or Keep only makes an edit.");
      if (review.suggestions.empty()) {
        if (review.totalCount > 0)
          append("No suggestions in this filter. Choose another filter.", true);
        else if (review.canUndoHide)
          append("No visible suggestions remain. Undo hide restores hidden items.", true);
        else
          append("No suggestions remain in the edited video.", true);
      }
      break;
    }
  }
  return presentation;
}

SuggestionPanelLayout layoutSuggestionPanel(const EditSnapshot& edit,
                                             int columns, int rows,
                                             int chromeTopY) {
  SuggestionPanelLayout panel;
  if (!edit.active || !edit.suggestionReview.visible || columns < 24 || rows < 8)
    return panel;
  const int bottom = chromeTopY > 0 ? chromeTopY - 1 : rows - 3;
  if (bottom < 5) return panel;
  panel.y = 1;
  panel.height = bottom - panel.y;
  panel.width = columns >= 100 ? std::clamp(columns / 2, 44, 68) : columns - 2;
  panel.x = columns - panel.width - 1;
  const int width = panel.width - 4;
  panel.closeColumn = panel.width - 9;
  std::vector<SuggestionPanelLayout::Line> body;
  const auto append = [&](const std::string& text, bool accent = false,
                          std::optional<uint64_t> id = {}, bool selected = false) {
    for (auto line : utf8WrapDisplayWidth(text, width))
      body.push_back({std::move(line), accent, id, selected});
  };
  for (const auto& paragraph : buildSuggestionPresentation(edit).introduction)
    append(paragraph.text, paragraph.accent);
  const auto& review = edit.suggestionReview;
  for (const auto& item : review.suggestions) {
    append(std::string(item.selected ? "> " : "") +
               sceneSuggestionKindLabel(item.kind) + " | Source " +
               duration(item.source.startUs) + " - " + duration(item.source.endUs) +
               " (" + duration(item.source.durationUs()) + ")",
           true, item.id, item.selected);
    append(item.reason, false, item.id, item.selected);
    body.push_back({});
  }
  // Fixed title and close button; all explanatory text and results are scrollable.
  const int page = std::max(1, panel.height - 3);
  panel.maximumScrollOffset = std::max(0, static_cast<int>(body.size()) - page);
  int offset = review.scrollOffset;
  if (offset < 0) {
    const auto selected = std::find_if(body.begin(), body.end(),
        [](const auto& line) { return line.selected; });
    const int index = static_cast<int>(selected - body.begin());
    offset = selected != body.end() && index >= page ? index : 0;
  }
  panel.scrollOffset = std::clamp(offset, 0, panel.maximumScrollOffset);
  panel.lines.push_back({kSuggestionPanelTitle, true, std::nullopt, false});
  const int end = std::min<int>(body.size(), panel.scrollOffset + page);
  panel.lines.insert(panel.lines.end(), body.begin() + panel.scrollOffset, body.begin() + end);
  return panel;
}

}  // namespace playback_video_edit
