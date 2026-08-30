#include "app/playback_control_router.h"

namespace application_playback {

PlaybackControlRouter::PlaybackControlRouter(
    audio_playback::Session& audioPlayback)
    : audioPlayback_(audioPlayback) {}

PlaybackControlSessionId PlaybackControlRouter::beginSession() {
  ++lastSessionValue_;
  if (lastSessionValue_ == 0) ++lastSessionValue_;
  sessionId_ = {lastSessionValue_};
  return sessionId_;
}

void PlaybackControlRouter::endSession() { sessionId_ = {}; }

PlaybackControlSessionId PlaybackControlRouter::sessionId() const noexcept {
  return sessionId_;
}

std::optional<PlaybackControlState>
PlaybackControlRouter::videoControlState(
    ConstVideoSessionRef videoSession) const {
  if (!sessionId_.valid() || !videoSession ||
      !videoSession->get().ready()) {
    return std::nullopt;
  }
  PlaybackControlState control = videoSession->get().controlState();
  control.session = sessionId_;
  return control;
}

ControlDispatch PlaybackControlRouter::dispatch(
    PlaybackControlCommand command, VideoSessionRef videoSession) {
  if (!sessionId_.valid()) return ControlUnhandled{};
  if (videoSession) {
    const bool handled = videoSession->get().handleControlCommand(command);
    return VideoControlDispatched{
        handled, handled && command == PlaybackControlCommand::Stop};
  }

  const AudioPlaybackSnapshot audio = audioPlayback_.snapshot();
  const bool hasTarget = audio.source.has_value();
  switch (command) {
    case PlaybackControlCommand::Play:
      if (!hasTarget) return ControlUnhandled{};
      audioPlayback_.play();
      return ControlApplied{};
    case PlaybackControlCommand::Pause:
      if (!hasTarget) return ControlUnhandled{};
      audioPlayback_.pause();
      return ControlApplied{};
    case PlaybackControlCommand::TogglePause:
      if (!hasTarget) return ControlUnhandled{};
      audioPlayback_.togglePause();
      return ControlApplied{};
    case PlaybackControlCommand::Stop:
      if (!audio.ready) return ControlUnhandled{};
      audioPlayback_.stop();
      endSession();
      return ControlApplied{};
    case PlaybackControlCommand::Previous:
      if (!hasTarget) return ControlUnhandled{};
      return QueueTransportRequested{playback_queue::Direction::Previous};
    case PlaybackControlCommand::Next:
      if (!hasTarget) return ControlUnhandled{};
      return QueueTransportRequested{playback_queue::Direction::Next};
  }
  return ControlUnhandled{};
}

ControlDispatch PlaybackControlRouter::dispatch(
    const PlaybackControlCommandEvent& event,
    VideoSessionRef videoSession) {
  if (!event.session.valid() || event.session != sessionId_) {
    return ControlUnhandled{};
  }
  return dispatch(event.command, videoSession);
}

bool PlaybackControlRouter::seekToRatio(double ratio,
                                        VideoSessionRef videoSession) {
  if (!sessionId_.valid()) return false;
  if (videoSession) return videoSession->get().seekToRatio(ratio);
  if (!audioPlayback_.snapshot().ready) return false;
  audioPlayback_.seekToRatio(ratio);
  return true;
}

}  // namespace application_playback
