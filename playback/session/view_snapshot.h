#pragma once

#include "playback/control/system_control_state.h"
#include "playback/session/presentation_policy.h"

namespace playback_session {

// Coherent application-facing view of one controllable video session. The
// session produces both values together so shell presentation never combines
// transport and window state from different protocol reads.
struct ViewSnapshot {
  PlaybackControlState control;
  PlaybackPresentationState presentation;
};

}  // namespace playback_session
