#pragma once

#include "tui/ui/playback_presentation.h"

class TuiMediaCoordinator;
class AudioPlaybackRuntime;

// Read-only application adapter. It samples each runtime once, then delegates
// all precedence and view-model policy to the pure presentation projection.
class TuiPlaybackPresenter {
 public:
  TuiPlaybackPresenter(const TuiMediaCoordinator& coordinator,
                       const AudioPlaybackRuntime& audioPlayback)
      : coordinator_(coordinator), audioPlayback_(audioPlayback) {}

  PlaybackPresentationModel model() const;

 private:
  const TuiMediaCoordinator& coordinator_;
  const AudioPlaybackRuntime& audioPlayback_;
};
