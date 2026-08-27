#include "playback/media_action_catalog.h"

namespace playback_media_actions {

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
    if (context.subtitleGenerationRunningForSource) {
      items.push_back(
          {Action::CancelSubtitleGeneration, "Cancel subtitle generation"});
    } else if (!context.backgroundTaskRunning) {
      items.push_back(
          {Action::GenerateSubtitles,
           context.hasGeneratedSubtitles ? "Regenerate subtitles..."
                                         : "Generate subtitles..."});
    }
    if (context.transcriptTextExportRunningForSource) {
      items.push_back({Action::CancelMediaExport,
                       "Cancel transcript export"});
    } else if (!context.backgroundTaskRunning &&
               context.canExportTranscriptText) {
      items.push_back(
          {Action::ExportTranscriptText, "Export transcript as text"});
    }
    if (context.audioExportRunningForSource) {
      items.push_back({Action::CancelMediaExport, "Cancel audio export"});
    } else if (!context.backgroundTaskRunning && context.canExportAudio) {
      items.push_back({Action::ExportAudio, "Export audio as FLAC"});
    }
    if (context.audioSeparationRunningForSource) {
      items.push_back(
          {Action::CancelAudioSeparation, "Cancel audio separation"});
    } else if (!context.backgroundTaskRunning && context.canSeparateAudio) {
      items.push_back(
          {Action::SeparateAudio,
           context.hasSeparatedAudio ? "Separate audio again..."
                                     : "Separate audio..."});
    }
    return items;
  }

  if (context.mediaKind != MediaKind::Audio) return items;
  if (!context.currentPlayback) {
    items.push_back({Action::Play, "Play"});
  }
  if (context.canBrowseTracks) {
    items.push_back({Action::BrowseTracks, "Browse tracks"});
  }
  if (context.audioExportRunningForSource) {
    items.push_back({Action::CancelMediaExport, "Cancel audio export"});
  } else if (!context.backgroundTaskRunning && context.canExportAudio) {
    items.push_back({Action::ExportAudio, "Export audio as FLAC"});
  }
  if (context.audioSeparationRunningForSource) {
    items.push_back(
        {Action::CancelAudioSeparation, "Cancel audio separation"});
  } else if (!context.backgroundTaskRunning && context.canSeparateAudio) {
    items.push_back(
        {Action::SeparateAudio,
         context.hasSeparatedAudio ? "Separate audio again..."
                                   : "Separate audio..."});
  }
  if (!context.backgroundTaskRunning) {
    if (context.canAnalyzeAudio) {
      items.push_back({Action::AnalyzeAudio, "Analyze"});
    }
    items.push_back({Action::SplitLoop, "Split loop"});
  }
  return items;
}

}  // namespace playback_media_actions
