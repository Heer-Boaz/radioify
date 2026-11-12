#include "overlay.h"

#include "playback/video/edit/overlay_model.h"

namespace playback_overlay {

PlaybackOverlayState
buildPlaybackOverlayState(const PlaybackOverlayInputs &inputs) {
  PlaybackOverlayState state;
  state.windowTitle = inputs.windowTitle;
  state.audioOk = inputs.audioOk;
  state.playPauseAvailable = inputs.playPauseAvailable;
  state.audioSupports50HzToggle = inputs.audioSupports50HzToggle;
  state.canPlayPrevious = inputs.canPlayPrevious;
  state.canPlayNext = inputs.canPlayNext;
  state.radioEnabled = inputs.radioEnabled;
  state.radioLabel = inputs.radioLabel;
  state.radioVisualizationAvailable = inputs.radioVisualizationAvailable;
  state.radioVisualizationActive = inputs.radioVisualizationActive;
  state.hz50Enabled = inputs.hz50Enabled;
  state.canCycleAudioTracks = inputs.canCycleAudioTracks;
  state.activeAudioTrackLabel = inputs.activeAudioTrackLabel;
  state.hasSubtitles = inputs.hasSubtitles;
  state.subtitlesEnabled = inputs.subtitlesEnabled;
  state.activeSubtitleLabel = inputs.subtitle.activeTrackLabel;
  state.subtitleClockUs = inputs.subtitleClockUs;
  state.seekingOverlay = inputs.seekingOverlay;
  state.displaySec = inputs.displaySec;
  state.totalSec = inputs.totalSec;
  state.volPct = inputs.volPct;
  state.overlayVisible = inputs.osd.controlsVisible;
  state.transientMessage = inputs.osd.message;
  state.paused = inputs.paused;
  state.pictureInPictureAvailable = inputs.pictureInPictureAvailable;
  state.pictureInPictureActive = inputs.pictureInPictureActive;
  state.subtitleText = inputs.subtitle.text;
  state.subtitleAssScript = inputs.subtitle.assScript;
  state.subtitleAssFonts = inputs.subtitle.assFonts;
  state.subtitleCues = inputs.subtitle.cues;
  state.subtitleRenderError = inputs.subtitleRenderError;
  state.debugLines = inputs.debugLines;
  state.contextMenu = inputs.contextMenu;
  state.videoEdit = inputs.videoEdit;
  state.videoEditExport = inputs.videoEditExport;
  state.videoEditPrompt = inputs.videoEditPrompt;
  state.mediaActionConfirmationPrompt = inputs.mediaActionConfirmationPrompt;
  state.mediaTaskActivity = inputs.mediaTaskActivity;
  state.chromeVisible =
      state.overlayVisible || !state.debugLines.empty() ||
      state.mediaActionConfirmationPrompt.has_value() ||
      state.mediaTaskActivity.has_value() ||
      playback_video_edit::needsOverlayPresentation(
          state.videoEdit, state.videoEditExport, state.videoEditPrompt);

  return state;
}

} // namespace playback_overlay
