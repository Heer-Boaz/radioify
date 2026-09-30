#include "presentation_projector.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "playback/video/state/machine.h"

namespace playback_session {

playback_overlay::MediaActionConfirmationDialog
projectMediaActionConfirmationDialog(
    const MediaActionConfirmationPrompt &prompt) {
  playback_overlay::MediaActionConfirmationDialog dialog;
  playback_media_confirmation::Content content =
      mediaActionConfirmationContent(prompt);
  dialog.title = std::move(content.title);
  dialog.text = std::move(content.text);
  dialog.primaryLabel = std::move(content.primaryLabel);
  dialog.secondaryLabel = std::move(content.secondaryLabel);
  dialog.selected =
      prompt.selected == MediaActionConfirmationChoice::Primary
          ? playback_overlay::MediaActionConfirmationSelection::Primary
          : playback_overlay::MediaActionConfirmationSelection::Secondary;
  return dialog;
}

playback_overlay::PlaybackOverlayState
projectPlaybackOverlay(OverlayProjection projection) {
  const auto &media = projection.media;
  const bool playerPaused =
      playback_video_state_machine::project(media.debug.state).transport ==
      playback_video_state_machine::TransportState::Paused;

  playback_overlay::PlaybackOverlayInputs inputs;
  inputs.windowTitle = media.windowTitle;
  inputs.audioOk = projection.audioOk;
  inputs.playPauseAvailable =
      projection.playbackState == PlaybackSessionState::Active ||
      projection.playbackState == PlaybackSessionState::Paused ||
      projection.playbackState == PlaybackSessionState::Ended;
  inputs.audioSupports50HzToggle =
      projection.audioOk && media.audio.supports50HzToggle;
  inputs.canPlayPrevious = projection.canPlayPrevious;
  inputs.canPlayNext = projection.canPlayNext;
  inputs.radioEnabled = media.audio.radioEnabled;
  inputs.radioLabel = media.audio.radioFilterLabel;
  inputs.hz50Enabled = media.audio.hz50Enabled;
  inputs.canCycleAudioTracks = projection.audioOk && media.canCycleAudioTracks;
  inputs.activeAudioTrackLabel = media.activeAudioTrackLabel;
  inputs.hasSubtitles = media.hasSubtitles;
  inputs.subtitlesEnabled = media.subtitlesEnabled;
  inputs.volPct = static_cast<int>(std::round(media.audio.volume * 100.0f));
  inputs.osd = std::move(projection.osd);
  inputs.paused = projection.playbackState == PlaybackSessionState::Paused ||
                  projection.playbackState == PlaybackSessionState::Ended ||
                  media.ended || playerPaused;
  inputs.pictureInPictureAvailable = projection.pictureInPictureAvailable;
  inputs.pictureInPictureActive = projection.pictureInPictureActive;
  inputs.subtitleRenderError = std::move(projection.subtitleRenderError);
  inputs.debugLines = std::move(projection.debugLines);
  inputs.contextMenu = std::move(projection.contextMenu);
  inputs.videoEdit = std::move(projection.videoEdit);
  inputs.videoEditExport = std::move(projection.videoEditExport);
  inputs.videoEditPrompt = projection.videoEditPrompt;
  inputs.mediaTaskActivity = std::move(projection.mediaTaskActivity);
  if (projection.mediaActionConfirmationPrompt) {
    inputs.mediaActionConfirmationPrompt = projectMediaActionConfirmationDialog(
        *projection.mediaActionConfirmationPrompt);
  }
  auto overlay = playback_overlay::buildPlaybackOverlayState(inputs);
  projectPlaybackTimeline(overlay, media, media.timeline);
  return overlay;
}

void projectPlaybackTimeline(
    playback_overlay::PlaybackOverlayState &overlay,
    const playback_screen_renderer::PlaybackMediaPresentation &media,
    const PlayerTimelineSnapshot &timeline) {
  double displaySec =
      static_cast<double>(std::max<int64_t>(0, timeline.positionUs)) / 1000000.0;
  const double totalSec =
      media.durationUs > 0
          ? static_cast<double>(media.durationUs) / 1000000.0
          : (overlay.audioOk ? media.audio.durationSec : -1.0);
  if (totalSec > 0.0) displaySec = std::clamp(displaySec, 0.0, totalSec);

  auto subtitle = playback_overlay::projectSubtitlePresentation(
      media.subtitleTrack.get(), media.subtitlesEnabled, timeline.seekPending(),
      timeline.sourcePositionUs, media.hasSubtitles);
  overlay.subtitleClockUs = timeline.sourcePositionUs;
  overlay.seekingOverlay = timeline.seekPending();
  overlay.displaySec = displaySec;
  overlay.totalSec = totalSec;
  overlay.activeSubtitleLabel = std::move(subtitle.activeTrackLabel);
  overlay.subtitleText = std::move(subtitle.text);
  overlay.subtitleAssScript = std::move(subtitle.assScript);
  overlay.subtitleAssFonts = std::move(subtitle.assFonts);
  overlay.subtitleCues = std::move(subtitle.cues);
  if (overlay.videoEdit.active) {
    overlay.videoEdit.playheadTimelineUs = timeline.positionUs;
  }
}

WindowUiState projectWindowUiState(
    const playback_screen_renderer::PlaybackScreenModel &playback,
    const PlayerTimelineSnapshot &timeline) {
  auto overlay = playback.overlay;
  projectPlaybackTimeline(overlay, playback.media, timeline);
  WindowUiState ui = playback_overlay::buildWindowUiState(
      overlay, playback.controlHoverToken);
  ui.timelinePreview = playback.timelinePreview;
  return ui;
}

} // namespace playback_session
