#include "playback/media_action_catalog.h"

namespace playback_media_actions {

namespace {

void appendAudioSeparationActions(std::vector<Item>& items,
                                  const Context& context) {
  using Availability =
      playback_media_processing::AudioSeparationAvailability;
  if (context.audioSeparationSetupRunningForSource &&
      context.activeTaskCancellable) {
    items.push_back({Action::CancelAudioSeparationSetup,
                     "Cancel audio separation setup"});
  } else if (context.audioSeparationRunningForSource &&
             context.activeTaskCancellable) {
    items.push_back(
        {Action::CancelAudioSeparation, "Cancel audio separation"});
  } else if (!context.backgroundTaskRunning &&
             context.audioSeparationAvailability ==
                 Availability::SetupRequired) {
    items.push_back(
        {Action::SetUpAudioSeparation, "Set up audio separation..."});
  } else if (!context.backgroundTaskRunning &&
             context.audioSeparationAvailability == Availability::Ready) {
    items.push_back(
        {Action::SeparateAudio,
         context.hasSeparatedAudio ? "Separate audio again..."
                                   : "Separate audio..."});
  }
}

}  // namespace

std::vector<Item> build(const Context& context) {
  std::vector<Item> items;
  if (context.mediaKind == MediaKind::Video) {
    if (!context.currentPlayback) {
      items.push_back({Action::Play, "Play"});
    }
    if (!context.editorActive) {
      items.push_back({Action::EditVideo,
                       context.hasEdits ? "Resume editing" : "Edit video"});
    }
    if (context.subtitleGenerationRunningForSource &&
        context.activeTaskCancellable) {
      items.push_back(
          {Action::CancelSubtitleGeneration, "Cancel transcript generation"});
    } else if (!context.backgroundTaskRunning &&
               context.canGenerateSubtitles) {
      items.push_back(
          {Action::GenerateSubtitles,
           context.hasGeneratedSubtitles ? "Regenerate transcript..."
                                         : "Generate transcript..."});
    }
    if (context.transcriptTextExportRunningForSource &&
        context.activeTaskCancellable) {
      items.push_back({Action::CancelMediaExport,
                       "Cancel transcript export"});
    } else if (!context.backgroundTaskRunning &&
               context.canExportTranscriptText) {
      items.push_back(
          {Action::ExportTranscriptText, "Export transcript as text"});
    }
    if (context.audioExportRunningForSource &&
        context.activeTaskCancellable) {
      items.push_back({Action::CancelMediaExport, "Cancel audio export"});
    } else if (!context.backgroundTaskRunning && context.canExportAudio) {
      items.push_back({Action::ExportAudio, "Export audio as FLAC"});
    }
    appendAudioSeparationActions(items, context);
    return items;
  }

  if (context.mediaKind != MediaKind::Audio) return items;
  if (!context.currentPlayback) {
    items.push_back({Action::Play, "Play"});
  }
  if (context.canBrowseTracks) {
    items.push_back({Action::BrowseTracks, "Browse tracks"});
  }
  if (context.audioExportRunningForSource &&
      context.activeTaskCancellable) {
    items.push_back({Action::CancelMediaExport, "Cancel audio export"});
  } else if (!context.backgroundTaskRunning && context.canExportAudio) {
    items.push_back({Action::ExportAudio, "Export audio as FLAC"});
  }
  appendAudioSeparationActions(items, context);
  if (!context.backgroundTaskRunning) {
    if (context.canAnalyzeAudio) {
      items.push_back({Action::AnalyzeAudio, "Analyze"});
    }
    items.push_back({Action::SplitLoop, "Split loop"});
  }
  return items;
}

}  // namespace playback_media_actions
