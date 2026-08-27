#pragma once

#include "tui/ui/playback_presentation.h"

class TuiMediaCoordinator;

// Read-only application adapter. It samples each runtime once, then delegates
// all precedence and view-model policy to the pure presentation projection.
class TuiPlaybackPresenter {
 public:
  explicit TuiPlaybackPresenter(const TuiMediaCoordinator& coordinator)
      : coordinator_(coordinator) {}

  PlaybackPresentationModel model() const;

 private:
  const TuiMediaCoordinator& coordinator_;
};
