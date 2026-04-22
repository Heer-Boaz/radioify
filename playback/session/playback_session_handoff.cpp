#include "playback_session_handoff.h"

#include <atomic>

#include "playback_session_state.h"

namespace playback_session_handoff {
namespace {

void finishHandoff(const playback_session_input::PlaybackInputView& view,
                   playback_session_input::PlaybackInputSignals& signals) {
  if (view.playbackState) {
    *view.playbackState = PlaybackSessionState::Exiting;
  }
  if (signals.loopStopRequested) {
    *signals.loopStopRequested = true;
  }
  if (signals.overlayUntilMs) {
    signals.overlayUntilMs->store(0, std::memory_order_relaxed);
  }
  if (signals.redraw) {
    *signals.redraw = true;
  }
  if (signals.forceRefreshArt) {
    *signals.forceRefreshArt = true;
  }
}

}  // namespace

bool requestTransportHandoff(
    const playback_session_input::PlaybackInputView& view,
    playback_session_input::PlaybackInputSignals& signals,
    PlaybackTransportCommand command) {
  if (!signals.requestTransportCommand ||
      !signals.requestTransportCommand(command)) {
    return false;
  }
  finishHandoff(view, signals);
  return true;
}

bool requestOpenFilesHandoff(
    const playback_session_input::PlaybackInputView& view,
    playback_session_input::PlaybackInputSignals& signals,
    const std::vector<std::filesystem::path>& files) {
  if (files.empty() || !signals.requestOpenFiles ||
      !signals.requestOpenFiles(files)) {
    return false;
  }
  finishHandoff(view, signals);
  return true;
}

}  // namespace playback_session_handoff
