#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "core/native_wait_handle.h"
#include "consolescreen.h"
#include "playback/video/gpu/gpu_shared.h"
#include "playback/overlay/overlay.h"
#include "playback/video/player.h"
#include "playback/video/framebuffer/window/window.h"

class ThreadDispatchQueue;
enum class PlaybackSessionState : uint8_t;

namespace playback_framebuffer_presenter {

enum class WindowThreadState : uint8_t {
  Disabled,
  Enabled,
  Stopping,
};

// Immutable session-owned state published to the presenter thread as one
// revision. Controls are projected only after this complete snapshot exists.
struct PlaybackFramebufferUiSnapshot {
  playback_overlay::PlaybackOsdSnapshot osd;
  playback_video_timeline_preview::Snapshot timelinePreview;
  playback_video_edit::EditSnapshot videoEdit;
  playback_video_edit::ExportProgress videoEditExport;
  playback_video_edit::Prompt videoEditPrompt =
      playback_video_edit::Prompt::None;
  playback_overlay::ContextMenuSnapshot contextMenu;
};

struct TextGridPresentationRequest {
  VideoWindow& videoWindow;
  int pixelWidth = 0;
  int pixelHeight = 0;
  int cellPixelWidth = 0;
  int cellPixelHeight = 0;
  const VideoFrame* frame = nullptr;
  bool frameChanged = false;
  const std::string& enhancementDebugLine;
};

struct TextGridPresentationTarget {
  std::vector<ScreenCell>& cells;
  int& cols;
  int& rows;
  playback_overlay::InteractionMap& interactions;
};

// Session-owned, thread-safe read model consumed by the window presenter.
// The presenter keeps this object alive for the complete lifetime of its
// worker thread; implementations must publish immutable revisions rather than
// reading mutable session-loop state directly.
class PresentationSource {
 public:
  virtual ~PresentationSource() = default;

  virtual WindowUiState buildWindowUiState(VideoWindow& videoWindow) = 0;
  virtual bool renderTextGrid(const TextGridPresentationRequest& request,
                              TextGridPresentationTarget target) = 0;
};

WindowUiState buildPlaybackFramebufferUiState(
    const std::string& windowTitle, VideoWindow& videoWindow, Player& player,
    SubtitleManager& subtitleManager, PlaybackSessionState playbackState,
    bool audioOk, bool canPlayPrevious, bool canPlayNext, bool hasSubtitles,
    std::atomic<bool>& enableSubtitlesShared,
    std::atomic<int>& overlayControlHover,
    const PlaybackFramebufferUiSnapshot& snapshot, bool debugOverlay);

void runFramebufferPresenterLoop(
    Player& player, VideoWindow& videoWindow, GpuVideoFrameCache& frameCache,
    std::atomic<WindowThreadState>& threadState,
    std::atomic<bool>& forcePresent, NativeWaitHandle wakeEvent,
    ThreadDispatchQueue& dispatch,
    PresentationSource& presentationSource);

}  // namespace playback_framebuffer_presenter
