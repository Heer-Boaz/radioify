#include "presentation_projector.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include "playback/video/player.h"
#include "playback/video/state/machine.h"

namespace playback_session {

playback_screen_renderer::PlaybackMediaPresentation capturePlaybackMedia(
    const Player& player, const AudioPlaybackSnapshot& audio,
    PlayerTimelineSnapshot timeline, std::string windowTitle, bool audioOk,
    bool hasSubtitles, bool subtitlesEnabled,
    playback_overlay::SubtitlePresentation subtitle) {
  playback_screen_renderer::PlaybackMediaPresentation media;
  media.windowTitle = std::move(windowTitle);
  media.timeline = std::move(timeline);
  media.debug = player.debugInfo();
  media.durationUs = player.durationUs();
  media.sourceWidth = player.sourceWidth();
  media.sourceHeight = player.sourceHeight();
  media.audioTrackCount = player.audioTrackCount();
  media.ended = player.isEnded();
  media.canCycleAudioTracks = player.canCycleAudioTracks();
  media.activeAudioTrackLabel =
      audioOk ? player.activeAudioTrackLabel() : "N/A";
  media.audio.streamClockReady = audio.streamClockReady;
  media.audio.streamStarved = audio.streamStarved;
  media.audio.finished = audio.finished;
  media.audio.supports50HzToggle = audio.supports50HzToggle;
  media.audio.radioEnabled = audio.radioEnabled;
  media.audio.hz50Enabled = audio.hz50Enabled;
  media.audio.durationSec = audio.durationSec;
  media.audio.volume = audio.volume;
  media.audio.radioFilterLabel = std::string(audio.radioFilterLabel);
  media.hasSubtitles = hasSubtitles;
  media.subtitlesEnabled = subtitlesEnabled;
  media.subtitle = std::move(subtitle);
  return media;
}

playback_overlay::PlaybackOverlayState projectPlaybackOverlay(
    OverlayProjection projection) {
  const auto& media = projection.media;
  double displaySec = media.timeline.positionUs > 0
                          ? static_cast<double>(media.timeline.positionUs) /
                                1000000.0
                          : 0.0;
  double totalSec =
      media.durationUs > 0
          ? static_cast<double>(media.durationUs) / 1000000.0
          : (projection.audioOk ? media.audio.durationSec : -1.0);
  if (totalSec > 0.0) {
    displaySec = std::clamp(displaySec, 0.0, totalSec);
  }
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
  inputs.canCycleAudioTracks =
      projection.audioOk && media.canCycleAudioTracks;
  inputs.activeAudioTrackLabel = media.activeAudioTrackLabel;
  inputs.subtitle = media.subtitle;
  inputs.hasSubtitles = media.hasSubtitles;
  inputs.subtitlesEnabled = media.subtitlesEnabled;
  inputs.subtitleClockUs = media.timeline.sourcePositionUs;
  inputs.seekingOverlay = media.timeline.seekPending();
  inputs.displaySec = displaySec;
  inputs.totalSec = totalSec;
  inputs.volPct =
      static_cast<int>(std::round(media.audio.volume * 100.0f));
  inputs.osd = std::move(projection.osd);
  inputs.paused =
      projection.playbackState == PlaybackSessionState::Paused ||
      projection.playbackState == PlaybackSessionState::Ended ||
      media.ended || playerPaused;
  inputs.pictureInPictureAvailable =
      projection.pictureInPictureAvailable;
  inputs.pictureInPictureActive = projection.pictureInPictureActive;
  inputs.subtitleRenderError =
      std::move(projection.subtitleRenderError);
  inputs.debugLines = std::move(projection.debugLines);
  inputs.contextMenu = std::move(projection.contextMenu);
  inputs.videoEdit = std::move(projection.videoEdit);
  if (inputs.videoEdit.active) {
    inputs.videoEdit.playheadTimelineUs = media.timeline.positionUs;
  }
  inputs.videoEditExport = std::move(projection.videoEditExport);
  inputs.videoEditPrompt = projection.videoEditPrompt;
  return playback_overlay::buildPlaybackOverlayState(inputs);
}

}  // namespace playback_session
