#pragma once

#include <memory>
#include <mutex>

#include "playback/ascii/screen_renderer.h"
#include "playback/framebuffer/presenter.h"

namespace playback_session {

struct WindowPresentationModel {
  playback_overlay::PlaybackOsdSnapshot osd;
  playback_video_timeline_preview::Snapshot timelinePreview;
  playback_video_edit::EditSnapshot videoEdit;
  playback_video_edit::ExportProgress videoEditExport;
  playback_video_edit::Prompt videoEditPrompt =
      playback_video_edit::Prompt::None;
  playback_overlay::ContextMenuSnapshot contextMenu;
};

WindowUiState projectWindowUiState(
    const playback_screen_renderer::PlaybackScreenResources& resources,
    VideoWindow& videoWindow,
    const playback_screen_renderer::PlaybackScreenModel& playback,
    const WindowPresentationModel& window);

class PresentationModel final
    : public playback_framebuffer_presenter::PresentationSource {
 public:
  struct Dependencies {
    playback_screen_renderer::PlaybackScreenResources renderer;
    std::mutex& subtitleMutex;
  };

  struct Revision {
    WindowUiState window;
    WindowPresentationModel windowModel;
    playback_screen_renderer::PlaybackScreenModel textGrid;
  };

  explicit PresentationModel(Dependencies dependencies);
  ~PresentationModel() override;

  PresentationModel(const PresentationModel&) = delete;
  PresentationModel& operator=(const PresentationModel&) = delete;

  void publish(Revision revision);

  WindowUiState windowUiState() override;
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
