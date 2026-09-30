#pragma once

#include <memory>

#include "playback/ascii/screen_renderer.h"
#include "playback/framebuffer/presenter.h"

namespace playback_session {

class PresentationModel final
    : public playback_framebuffer_presenter::PresentationSource {
 public:
  struct Dependencies {
    playback_screen_renderer::PlaybackScreenResources renderer;
  };

  struct Revision {
    playback_screen_renderer::PlaybackScreenModel textGrid;
  };

  explicit PresentationModel(Dependencies dependencies);
  ~PresentationModel() override;

  PresentationModel(const PresentationModel&) = delete;
  PresentationModel& operator=(const PresentationModel&) = delete;

  void publish(Revision revision);

  WindowUiState windowUiState(const PlayerTimelineSnapshot &timeline) override;
  bool renderTextGrid(
      const playback_framebuffer_presenter::TextGridPresentationRequest&
          request,
      playback_framebuffer_presenter::TextGridPresentationTarget target)
      override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace playback_session
