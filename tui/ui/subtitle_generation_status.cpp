#include "tui/ui/subtitle_generation_status.h"

#include "core/runtime_helpers.h"

std::string subtitleGenerationStatus(
    const playback_video_transcript::GenerationJobSnapshot& snapshot) {
  using State = playback_video_transcript::GenerationJobState;
  switch (snapshot.state) {
    case State::Succeeded: {
      const std::string filename =
          toUtf8String(snapshot.outputFile.filename());
      return filename.empty() ? "Subtitles ready."
                              : "Subtitles ready: " + filename;
    }
    case State::Failed:
      return snapshot.error.empty() ? "Subtitle generation failed."
                                    : snapshot.error;
    case State::Cancelled:
      return "Subtitle generation cancelled.";
    case State::Idle:
    case State::Running:
    case State::Cancelling:
      return {};
  }
  return {};
}
