#include "playback/media_action_catalog.h"

#include "audio/media_formats.h"

namespace playback_media_actions {

MediaKind mediaKindForSource(const std::filesystem::path& sourceFile) {
  if (isSupportedVideoExt(sourceFile)) return MediaKind::Video;
  if (isSupportedAudioExt(sourceFile)) return MediaKind::Audio;
  return MediaKind::Unsupported;
}

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
