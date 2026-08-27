#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "playback/framebuffer/presenter.h"
#include "playback/session/state.h"

class Player;
class SubtitleManager;

namespace playback_screen_renderer {
struct PlaybackScreenRenderInputs;
}

namespace playback_session {

class PresentationModel final
    : public playback_framebuffer_presenter::PresentationSource {
 public:
  struct Dependencies {
    Player& player;
    SubtitleManager& subtitleManager;
    std::mutex& subtitleMutex;
    std::atomic<bool>& subtitlesEnabled;
    std::atomic<int>& controlHover;
  };

  struct FixedState {
    std::string windowTitle;
    bool canPlayPrevious = false;
    bool canPlayNext = false;
  };

  PresentationModel(Dependencies dependencies, FixedState fixedState);
  ~PresentationModel() override;

  PresentationModel(const PresentationModel&) = delete;
  PresentationModel& operator=(const PresentationModel&) = delete;

  void publishWindowState(
      playback_framebuffer_presenter::PlaybackFramebufferUiSnapshot ui,
      PlaybackSessionState playbackState, bool audioOk, bool audioStarting,
      bool hasSubtitles, bool debugOverlay);
  void publishTextGridInputs(
      const playback_screen_renderer::PlaybackScreenRenderInputs& inputs);

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
