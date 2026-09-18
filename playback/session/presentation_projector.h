#pragma once

#include <string>
#include <vector>

#include "audio/playback_snapshot.h"
#include "playback/ascii/screen_renderer.h"
#include "playback/session/media_action_confirmation.h"

class Player;

namespace playback_session {

playback_screen_renderer::PlaybackMediaPresentation
capturePlaybackMedia(const Player &player, const AudioPlaybackSnapshot &audio,
                     PlayerTimelineSnapshot timeline, std::string windowTitle,
                     bool audioOk, bool hasSubtitles, bool subtitlesEnabled,
                     playback_overlay::SubtitlePresentation subtitle);

struct OverlayProjection {
  explicit OverlayProjection(
      const playback_screen_renderer::PlaybackMediaPresentation &media)
      : media(media) {}

  const playback_screen_renderer::PlaybackMediaPresentation &media;
  PlaybackSessionState playbackState = PlaybackSessionState::Active;
  bool audioOk = false;
  bool canPlayPrevious = false;
  bool canPlayNext = false;
  playback_overlay::PlaybackOsdSnapshot osd;
  bool pictureInPictureAvailable = false;
  bool pictureInPictureActive = false;
  std::string subtitleRenderError;
  std::vector<std::string> debugLines;
  playback_overlay::ContextMenuSnapshot contextMenu;
  playback_video_edit::EditSnapshot videoEdit;
  playback_video_edit::ExportProgress videoEditExport;
  playback_video_edit::Prompt videoEditPrompt =
      playback_video_edit::Prompt::None;
  std::optional<MediaActionConfirmationPrompt> mediaActionConfirmationPrompt;
  std::optional<playback_media_processing::Activity> mediaTaskActivity;
};

playback_overlay::PlaybackOverlayState
projectPlaybackOverlay(OverlayProjection projection);

playback_overlay::MediaActionConfirmationDialog
projectMediaActionConfirmationDialog(
    const MediaActionConfirmationPrompt &prompt);

} // namespace playback_session
