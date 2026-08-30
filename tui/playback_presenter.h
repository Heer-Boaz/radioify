#pragma once

#include "tui/ui/playback_presentation.h"

class TuiMediaCoordinator;

// Read-only application adapter. It projects one coordinator-owned playback
// snapshot so presentation never assembles state through independent getters.
class TuiPlaybackPresenter {
 public:
  explicit TuiPlaybackPresenter(const TuiMediaCoordinator& coordinator)
      : coordinator_(coordinator) {}

  PlaybackPresentationModel model() const;

 private:
  const TuiMediaCoordinator& coordinator_;
};
