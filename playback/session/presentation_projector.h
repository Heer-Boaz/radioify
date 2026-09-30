#pragma once

#include <string>
#include <vector>

#include "playback/ascii/screen_renderer.h"
#include "playback/session/media_action_confirmation.h"

namespace playback_session {

// Re-project time-dependent presentation from an immutable session revision
// and the presenter's current timeline, including subtitle cue boundaries.
void projectPlaybackTimeline(
    playback_overlay::PlaybackOverlayState &overlay,
    const playback_screen_renderer::PlaybackMediaPresentation &media,
    const PlayerTimelineSnapshot &timeline);

WindowUiState projectWindowUiState(
    const playback_screen_renderer::PlaybackScreenModel &playback,
    const PlayerTimelineSnapshot &timeline);

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
