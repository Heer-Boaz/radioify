#pragma once

#include <memory>
#include <mutex>

#include "playback/ascii/screen_renderer.h"
#include "playback/framebuffer/presenter.h"

namespace playback_session {

class PresentationModel final
    : public playback_framebuffer_presenter::PresentationSource {
 public:
  struct Dependencies {
    playback_screen_renderer::PlaybackScreenResources renderer;
    std::mutex& subtitleMutex;
  };

  struct Revision {
    playback_framebuffer_presenter::PlaybackFramebufferUiSnapshot window;
    playback_screen_renderer::PlaybackScreenModel textGrid;
  };

  explicit PresentationModel(Dependencies dependencies);
  ~PresentationModel() override;

  PresentationModel(const PresentationModel&) = delete;
  PresentationModel& operator=(const PresentationModel&) = delete;

  void publish(Revision revision);

  WindowUiState buildWindowUiState(VideoWindow& videoWindow) override;
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
