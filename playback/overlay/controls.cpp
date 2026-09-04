#include "playback/overlay/overlay.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#include "playback/video/edit/scene_suggestions.h"
#include "unicode_display_width.h"

namespace playback_overlay {
namespace {

int visibleWidth(const std::string &text) {
  std::string filtered;
  filtered.reserve(text.size());
  for (char c : text) {
    if (c != '\r' && c != '\n')
      filtered.push_back(c);
  }
  return utf8DisplayWidth(filtered);
}

void finishControlSpecs(std::vector<OverlayControlSpec> *specs,
                        int hoverControlToken) {
  if (!specs)
    return;
  for (OverlayControlSpec &spec : *specs) {
    const bool hovered =
        spec.enabled && overlayControlToken(spec.id) == hoverControlToken;
    spec.renderText = hovered ? spec.hoverText : spec.normalText;
    const int textWidth = visibleWidth(spec.renderText);
    if (textWidth < spec.width) {
      spec.renderText.append(static_cast<size_t>(spec.width - textWidth), ' ');
    } else if (textWidth > spec.width) {
      spec.renderText = utf8TakeDisplayWidth(spec.renderText, spec.width);
    }
  }
}

OverlayControlSpec makePlayPauseSpec(const PlaybackOverlayState &state) {
  OverlayControlSpec spec = makeOverlayTextControlSpec(
      OverlayControlId::PlayPause, state.paused ? "Play" : "Pause",
      state.paused, state.playPauseAvailable);
  const OverlayControlSpec widest =
      makeOverlayTextControlSpec(OverlayControlId::PlayPause, "Pause", false);
  spec.width = std::max(spec.width, widest.width);
  return spec;
}

} // namespace

OverlayControlSpec makeOverlayTextControlSpec(OverlayControlId id,
                                              const std::string &label,
                                              bool active, bool enabled) {
  OverlayControlSpec spec;
  spec.id = id;
  spec.normalText = " [" + label + "] ";
  spec.hoverText = "[ " + label + " ]";
  spec.width = std::max(utf8DisplayWidth(spec.normalText),
                        utf8DisplayWidth(spec.hoverText));
  spec.active = active;
  spec.enabled = enabled;
  return spec;
}

std::vector<float> indeterminateCharacterSweep(int width, double phase) {
  constexpr int kAfterglowCells = 4;
  const int safeWidth = std::max(0, width);
  std::vector<float> intensities(static_cast<std::size_t>(safeWidth), 0.0f);
  if (safeWidth == 0)
    return intensities;

  double normalizedPhase = std::isfinite(phase) ? phase - std::floor(phase)
                                                 : 0.0;
  if (normalizedPhase < 0.0)
    normalizedPhase += 1.0;
  const int head = static_cast<int>(std::floor(
      normalizedPhase * static_cast<double>(safeWidth + kAfterglowCells)));
  for (int column = 0; column < safeWidth; ++column) {
    const int distance = head - column;
    if (distance >= 0 && distance < kAfterglowCells) {
      intensities[static_cast<std::size_t>(column)] =
          1.0f - static_cast<float>(distance) / kAfterglowCells;
    }
  }
  return intensities;
}

std::vector<float> chapterControlCharacterHighlights(
    const playback_video_chapters::Snapshot &chapters, bool motionEnabled,
    int width, double phase) {
  if (!chapters.running())
    return {};
  if (motionEnabled)
    return indeterminateCharacterSweep(width, phase);
  return std::vector<float>(static_cast<std::size_t>(std::max(0, width)),
                            0.75f);
}

std::vector<OverlayCellControlInput>
buildOverlayCellControlInputs(const std::vector<OverlayControlSpec> &specs,
                              int hoverControlToken) {
  std::vector<OverlayCellControlInput> controls;
  controls.reserve(specs.size());
  for (const OverlayControlSpec &spec : specs) {
    const bool hovered =
        spec.enabled && overlayControlToken(spec.id) == hoverControlToken;
    OverlayCellControlInput control;
    control.id = spec.id;
    control.text = hovered ? spec.hoverText : spec.normalText;
    control.width = spec.width;
    control.active = spec.active;
    control.hovered = hovered;
    control.enabled = spec.enabled;
    control.tone = spec.tone;
    controls.push_back(std::move(control));
  }
  return controls;
}

std::vector<OverlayDialogButtonInput> buildMediaActionConfirmationDialogButtons(
    const MediaActionConfirmationDialog &prompt, int hoverControlToken) {
  using Selection = MediaActionConfirmationSelection;
  return {
      {OverlayControlId::MediaActionPrimary, prompt.primaryLabel,
       prompt.primaryLabel, prompt.selected == Selection::Primary,
       hoverControlToken ==
           overlayControlToken(OverlayControlId::MediaActionPrimary),
       true},
      {OverlayControlId::MediaActionSecondary, prompt.secondaryLabel,
       prompt.secondaryLabel, prompt.selected == Selection::Secondary,
       hoverControlToken ==
           overlayControlToken(OverlayControlId::MediaActionSecondary),
       true},
  };
}

OverlayControlIntent intentForOverlayControl(OverlayControlId id) {
  switch (id) {
  case OverlayControlId::Previous:
    return OverlayAction::Previous;
  case OverlayControlId::PlayPause:
    return OverlayAction::TogglePlayPause;
  case OverlayControlId::Next:
    return OverlayAction::Next;
  case OverlayControlId::Radio:
    return OverlayAction::ToggleRadio;
  case OverlayControlId::Hz50:
    return OverlayAction::Toggle50Hz;
  case OverlayControlId::AudioTrack:
    return OverlayAction::CycleAudioTrack;
  case OverlayControlId::Subtitles:
    return OverlayAction::ToggleSubtitles;
  case OverlayControlId::Chapters:
    return OverlayAction::ToggleChapterOverview;
  case OverlayControlId::ChapterOverviewClose:
    return OverlayAction::CloseChapterOverview;
  case OverlayControlId::ChapterInstall:
    return OverlayAction::InstallChapterModel;
  case OverlayControlId::ChapterCancel:
    return OverlayAction::CancelChapterOperation;
  case OverlayControlId::PictureInPicture:
    return OverlayAction::TogglePictureInPicture;
  case OverlayControlId::EditMarkIn:
    return playback_video_edit::Command::ToggleIn;
  case OverlayControlId::EditMarkOut:
    return playback_video_edit::Command::ToggleOut;
  case OverlayControlId::EditClearSelection:
    return playback_video_edit::Command::ClearInAndOut;
  case OverlayControlId::EditRippleDelete:
    return playback_video_edit::Command::RippleDelete;
  case OverlayControlId::EditTrim:
    return playback_video_edit::Command::Trim;
  case OverlayControlId::EditSuggestions:
    return playback_video_edit::Command::ToggleSceneSuggestions;
  case OverlayControlId::EditSuggestionFilter:
    return playback_video_edit::Command::CycleSceneSuggestionFilter;
  case OverlayControlId::EditPreviousSuggestion:
    return playback_video_edit::Command::PreviousSceneSuggestion;
  case OverlayControlId::EditNextSuggestion:
    return playback_video_edit::Command::NextSceneSuggestion;
  case OverlayControlId::EditSelectSuggestion:
    return playback_video_edit::Command::SelectSceneSuggestion;
  case OverlayControlId::EditHideSuggestion:
    return playback_video_edit::Command::DismissSceneSuggestion;
  case OverlayControlId::EditUndoHideSuggestion:
    return playback_video_edit::Command::UndoDismissSceneSuggestion;
  case OverlayControlId::EditDone:
    return playback_video_edit::Command::Finish;
  case OverlayControlId::EditStartExport:
    return playback_video_edit::Command::StartExport;
  case OverlayControlId::EditWaitForExport:
    return OverlayAction::WaitForVideoEditExport;
  case OverlayControlId::EditCancelExport:
    return playback_video_edit::Command::CancelExport;
  case OverlayControlId::EditConfirmPrompt:
    return playback_video_edit::Command::ConfirmPrompt;
  case OverlayControlId::EditCancelPrompt:
    return playback_video_edit::Command::CancelPrompt;
  case OverlayControlId::EditDiscardAndExit:
    return OverlayAction::ConfirmPendingExit;
  case OverlayControlId::EditCancelExit:
    return OverlayAction::CancelPendingExit;
  case OverlayControlId::MediaTaskCancel:
    return OverlayAction::CancelMediaTask;
  case OverlayControlId::MediaActionPrimary:
    return OverlayAction::ConfirmMediaAction;
  case OverlayControlId::MediaActionSecondary:
    return OverlayAction::DismissMediaAction;
  }
  throw std::invalid_argument("Unknown playback overlay control.");
}

std::vector<OverlayControlSpec>
buildOverlayControlSpecs(const PlaybackOverlayState &state,
                         int hoverControlToken,
                         const OverlayControlSpecOptions &options) {
  std::vector<OverlayControlSpec> out;
  const auto add = [&](OverlayControlId id, const std::string &label,
                       bool active, bool enabled = true) {
    out.push_back(makeOverlayTextControlSpec(id, label, active, enabled));
  };
  const auto finish = [&]() { finishControlSpecs(&out, hoverControlToken); };
  const auto addMediaTaskCancellation = [&]() {
    if (state.mediaTaskActivity && state.mediaTaskActivity->cancellable) {
      add(OverlayControlId::MediaTaskCancel, "Cancel task", false,
          !state.mediaTaskActivity->cancelling);
    }
  };

  if (state.mediaActionConfirmationPrompt) {
    for (const OverlayDialogButtonInput &button :
         buildMediaActionConfirmationDialogButtons(
             *state.mediaActionConfirmationPrompt, hoverControlToken)) {
      add(button.id, button.label, button.selected, button.enabled);
    }
    finish();
    return out;
  }

  if (state.videoEditPrompt == playback_video_edit::Prompt::LeaveEditMode) {
    add(OverlayControlId::EditConfirmPrompt, "Leave", true);
    add(OverlayControlId::EditCancelPrompt, "Cancel", false);
    finish();
    return out;
  }

  if (state.videoEditPrompt == playback_video_edit::Prompt::DiscardEdits) {
    add(OverlayControlId::EditConfirmPrompt, "Discard", false);
    add(OverlayControlId::EditCancelPrompt, "Cancel", true);
    finish();
    return out;
  }

  if (state.videoEditPrompt == playback_video_edit::Prompt::LeavePlayback) {
    const playback_video_edit::ExitExportAction exportAction =
        playback_video_edit::exitExportAction({
            state.videoEdit.hasUnexportedChanges,
            state.videoEditExport.running(),
            state.videoEditExport.targetsCurrentRevision,
        });
    switch (exportAction) {
    case playback_video_edit::ExitExportAction::ExportCurrent:
      add(OverlayControlId::EditStartExport,
          state.videoEditExport.failed() ? "Retry" : "Export", false);
      break;
    case playback_video_edit::ExitExportAction::WaitForExport:
      add(OverlayControlId::EditWaitForExport, "Wait", true);
      break;
    case playback_video_edit::ExitExportAction::CancelBlockingExport:
      break;
    case playback_video_edit::ExitExportAction::None:
      break;
    }
    if (state.videoEditExport.running()) {
      add(OverlayControlId::EditCancelExport, "Cancel export", false);
    } else {
      add(OverlayControlId::EditDiscardAndExit,
          state.videoEdit.hasUnexportedChanges ? "Discard" : "Exit", false);
    }
    add(OverlayControlId::EditCancelExit, "Stay", true);
    finish();
    return out;
  }

  if (state.videoEdit.active) {
    // Range selection is a local tool mode. Its operations lead the toolbar
    // only while a complete range exists; transport and document completion
    // remain available without turning the bar into a shortcut legend.
    const bool hasStart = state.videoEdit.inTimelineUs.has_value();
    const bool hasEnd = state.videoEdit.outTimelineUs.has_value();
    const bool hasMarks = hasStart || hasEnd;
    const bool completeRange = hasStart && hasEnd;
    const bool showPictureInPicture =
        options.includePictureInPicture && state.pictureInPictureAvailable;

    addMediaTaskCancellation();
    if (showPictureInPicture && state.pictureInPictureActive) {
      add(OverlayControlId::PictureInPicture, "Close PiP", true);
    }
    if (completeRange) {
      add(OverlayControlId::EditRippleDelete, "Remove", false,
          state.videoEdit.canRippleDelete);
      add(OverlayControlId::EditTrim, "Keep only", false,
          state.videoEdit.canTrim);
    } else {
      add(OverlayControlId::EditMarkIn, "Start", hasStart);
      add(OverlayControlId::EditMarkOut, "End", hasEnd);
    }
    if (hasMarks) {
      add(OverlayControlId::EditClearSelection, "Cancel", false);
    }
    const bool detectingSegments =
        state.videoEdit.sceneAnalysisStatus ==
        playback_video_edit::SceneAnalysisStatus::Running;
    const bool suggestionsReady =
        state.videoEdit.sceneAnalysisStatus ==
        playback_video_edit::SceneAnalysisStatus::Ready;
    const bool suggestionsFailed =
        state.videoEdit.sceneAnalysisStatus ==
        playback_video_edit::SceneAnalysisStatus::Failed;
    std::string suggestionsLabel;
    if (detectingSegments) {
      suggestionsLabel = "Cancel detection";
    } else if (suggestionsFailed) {
      suggestionsLabel = "Retry suggestions";
    } else if (suggestionsReady) {
      suggestionsLabel =
          "Suggestions " +
          std::to_string(state.videoEdit.suggestionReview.totalCount);
    } else {
      suggestionsLabel = "Suggestions";
    }
    add(OverlayControlId::EditSuggestions, suggestionsLabel,
        detectingSegments || state.videoEdit.suggestionReview.visible);

    if (suggestionsReady && state.videoEdit.suggestionReview.visible) {
      add(OverlayControlId::EditSuggestionFilter,
          std::string("Filter: ") +
              playback_video_edit::sceneSuggestionFilterLabel(
                  state.videoEdit.suggestionReview.filter),
          state.videoEdit.suggestionReview.filter !=
              playback_video_edit::SceneSuggestionFilter::All);
      const bool hasSuggestion =
          state.videoEdit.suggestionReview.selectedId.has_value();
      add(OverlayControlId::EditPreviousSuggestion, "Previous", false,
          hasSuggestion);
      add(OverlayControlId::EditNextSuggestion, "Next", false, hasSuggestion);
      add(OverlayControlId::EditSelectSuggestion, "Select range", false,
          hasSuggestion);
      add(OverlayControlId::EditHideSuggestion, "Hide", false, hasSuggestion);
      if (state.videoEdit.suggestionReview.canUndoHide) {
        add(OverlayControlId::EditUndoHideSuggestion, "Undo hide", false);
      }
    }
    out.push_back(makePlayPauseSpec(state));
    add(OverlayControlId::EditDone, "Done", false);
    if (showPictureInPicture && !state.pictureInPictureActive) {
      add(OverlayControlId::PictureInPicture, "PiP", false);
    }
    finish();
    return out;
  }

  addMediaTaskCancellation();
  if (state.canPlayPrevious) {
    add(OverlayControlId::Previous, "<<", false);
  }
  if (state.playPauseAvailable) {
    add(OverlayControlId::PlayPause, "pause", state.paused);
  }
  if (state.canPlayNext) {
    add(OverlayControlId::Next, ">>", false);
  }

  if (options.includeRadio) {
    add(OverlayControlId::Radio, state.radioLabel, state.radioEnabled);
  }

  if (state.audioOk && state.audioSupports50HzToggle) {
    add(OverlayControlId::Hz50, "50Hz", state.hz50Enabled);
  }

  if (options.includeAudioTrack) {
    std::string audioLabel = "Audio: N/A";
    if (state.canCycleAudioTracks && state.audioOk) {
      std::string activeAudio = state.activeAudioTrackLabel;
      if (activeAudio.empty())
        activeAudio = "N/A";
      if (utf8DisplayWidth(activeAudio) > 14) {
        activeAudio = utf8TakeDisplayWidth(activeAudio, 14);
      }
      audioLabel = "Audio: " + activeAudio;
    }
    add(OverlayControlId::AudioTrack, audioLabel,
        state.audioOk && state.canCycleAudioTracks);
  }

  if (options.includeSubtitles) {
    const bool subtitlesActive = state.hasSubtitles && state.subtitlesEnabled;
    std::string subtitleLabel = "Subs";
    if (subtitlesActive) {
      std::string activeSubtitle = state.activeSubtitleLabel;
      if (!activeSubtitle.empty() && activeSubtitle != "N/A") {
        if (utf8DisplayWidth(activeSubtitle) > 14) {
          activeSubtitle = utf8TakeDisplayWidth(activeSubtitle, 14);
        }
        subtitleLabel += ": " + activeSubtitle;
      }
    }
    add(OverlayControlId::Subtitles, subtitleLabel, subtitlesActive);
  }

  // Chapters are a stable player capability. Keep their entry point in one
  // place while its content moves through asynchronous lifecycle states; the
  // panel itself remains content-only and is opened only when data is ready.
  if (state.chapterControlVisible) {
    add(OverlayControlId::Chapters, "Chapters", state.chapterOverviewOpen);
    OverlayControlSpec &chapters = out.back();
    if (state.chapters.state ==
        playback_video_chapters::AnalysisState::Failed) {
      chapters.tone = OverlayControlTone::Error;
    } else if (state.chapters.state ==
                   playback_video_chapters::AnalysisState::SetupRequired ||
               state.chapters.state ==
                   playback_video_chapters::AnalysisState::Unsupported) {
      chapters.tone = OverlayControlTone::Warning;
    }
  }
  if (state.chapters.state ==
             playback_video_chapters::AnalysisState::SetupRequired) {
    add(OverlayControlId::ChapterInstall, "Install chapter models", false);
  } else if (state.chapters.state ==
             playback_video_chapters::AnalysisState::Installing) {
    add(OverlayControlId::ChapterCancel, "Cancel install", false);
  }

  if (options.includePictureInPicture && state.pictureInPictureAvailable) {
    add(OverlayControlId::PictureInPicture, "PiP",
        state.pictureInPictureActive);
  }

  finish();
  return out;
}

std::vector<OverlayControlSpec>
buildOverlayControlSpecs(const PlaybackOverlayState &state,
                         int hoverControlToken) {
  return buildOverlayControlSpecs(state, hoverControlToken,
                                  OverlayControlSpecOptions{});
}

} // namespace playback_overlay
