#include "tui/ui/audio_separation_status.h"

std::string audioSeparationStatus(
    const audio_separation::JobSnapshot& snapshot) {
  using State = audio_separation::JobState;
  switch (snapshot.state) {
    case State::Succeeded:
      return "Audio stems ready: dialogue, music and effects.";
    case State::Failed:
      return snapshot.error.empty() ? "Audio separation failed."
                                    : snapshot.error;
    case State::Cancelled:
      return "Audio separation cancelled.";
    case State::Idle:
    case State::Running:
    case State::Cancelling:
      return {};
  }
  return {};
}
