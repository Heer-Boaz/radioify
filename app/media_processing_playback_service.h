#pragma once

#include <filesystem>
#include <functional>

#include "app/media_processing_coordinator.h"
#include "playback/media_processing_actions.h"

namespace media_processing {

// Adapts the application-owned coordinator to the smaller contract consumed
// by browser and player surfaces. Accepted commands trigger one presentation
// invalidation without exposing task-specific lifecycle glue to the TUI.
class PlaybackService final : public playback_media_processing::Service {
 public:
  using StateChanged = std::function<void()>;

  PlaybackService(Coordinator& coordinator, StateChanged stateChanged);

  playback_media_processing::SourceState sourceStateFor(
      const std::filesystem::path& sourceFile) const override;
  bool requestSubtitles(
      const std::filesystem::path& sourceFile) override;
  bool requestSubtitleCancellation() override;
  bool requestAudioSeparation(
      const std::filesystem::path& sourceFile) override;
  bool requestAudioSeparationCancellation() override;

  bool requestActiveCancellation();

 private:
  bool publishAccepted(bool accepted);

  Coordinator& coordinator_;
  StateChanged stateChanged_;
};

}  // namespace media_processing
