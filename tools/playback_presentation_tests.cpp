#include "tui/ui/playback_presentation.h"

#include <cstdio>
#include <filesystem>
#include <limits>

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::fprintf(stderr, "playback_presentation_tests: %s\n", message);
  return false;
}

}  // namespace

int main() {
  bool ok = true;

  const PlaybackPresentationModel inactive = playbackPresentationModel(
      AudioPlaybackSnapshot{}, std::nullopt, std::nullopt);
  ok &= expect(!inactive.audioTarget && !inactive.currentTarget &&
                   !inactive.control && !inactive.videoPresentation,
               "inactive runtimes must project no synthetic target");

  AudioPlaybackSnapshot audio;
  audio.source = AudioPlaybackSource{std::filesystem::path("album.nsf"), 3};
  audio.ready = true;
  audio.paused = true;
  audio.positionSec = 12.5;
  audio.durationSec = 90.0;
  const PlaybackPresentationModel audioOnly = playbackPresentationModel(
      audio, std::nullopt, PlaybackPresentationState::nativeWindowed());
  ok &= expect(audioOnly.audioTarget && audioOnly.currentTarget &&
                   samePlaybackTarget(*audioOnly.audioTarget,
                                      *audioOnly.currentTarget) &&
                   playbackTargetTrackIndex(*audioOnly.currentTarget) == 3 &&
                   audioOnly.control && !audioOnly.control->isVideo &&
                   audioOnly.control->status ==
                       PlaybackControlStatus::Paused &&
                   audioOnly.control->positionSec == 12.5 &&
                   audioOnly.control->durationSec == 90.0 &&
                   !audioOnly.videoPresentation,
               "audio projection must own transport state and ignore orphaned "
               "video presentation");

  PlaybackControlState videoControl(
      playbackFileTarget(std::filesystem::path("movie.mp4")), true);
  videoControl.status = PlaybackControlStatus::Playing;
  PlaybackPresentationState videoPresentation =
      PlaybackPresentationState::nativeWindowed().togglePictureInPicture();
  const PlaybackPresentationModel video = playbackPresentationModel(
      audio, videoControl, videoPresentation);
  ok &= expect(video.audioTarget && video.currentTarget && video.control &&
                   video.control->isVideo &&
                   playbackTargetFile(*video.currentTarget) == "movie.mp4" &&
                   video.videoPresentation == videoPresentation,
               "video transport and presentation must take visible priority "
               "without discarding sampled audio state");

  AudioPlaybackSnapshot finishedAudio = audio;
  finishedAudio.paused = false;
  finishedAudio.finished = true;
  finishedAudio.durationSec = std::numeric_limits<double>::quiet_NaN();
  const PlaybackPresentationModel finished = playbackPresentationModel(
      finishedAudio, std::nullopt, std::nullopt);
  ok &= expect(finished.control &&
                   finished.control->status ==
                       PlaybackControlStatus::Stopped &&
                   !finished.control->durationSec,
               "finished and invalid-duration audio must project safely");

  return ok ? 0 : 1;
}
