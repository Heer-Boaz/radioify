#pragma once

#include <optional>

#include "audio/playback_snapshot.h"
#include "playback/control/system_control_state.h"
#include "playback/session/presentation_policy.h"
#include "playback/target.h"

struct PlaybackPresentationModel {
  AudioPlaybackSnapshot audio;
  std::optional<PlaybackTarget> audioTarget;
  std::optional<PlaybackTarget> currentTarget;
  std::optional<PlaybackControlState> control;
  std::optional<PlaybackPresentationState> videoPresentation;
};

// Pure projection used by every TUI playback surface. Video state takes
// presentation priority while the audio state remains available for meters and
// audio-only controls.
PlaybackPresentationModel playbackPresentationModel(
    AudioPlaybackSnapshot audio,
    std::optional<PlaybackControlState> videoControl,
    std::optional<PlaybackPresentationState> videoPresentation);
