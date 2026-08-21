#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
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

using TextGridPresentationProvider =
    std::function<bool(int pixelWidth, int pixelHeight, int cellPixelWidth,
                       int cellPixelHeight,
                       const VideoFrame* frame, bool frameChanged,
                       const std::string& enhancementDebugLine,
                       std::vector<ScreenCell>& outCells,
                       int& outCols, int& outRows,
                       playback_overlay::InteractionMap& outInteractions)>;

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
    const std::function<WindowUiState()>& buildUiState,
    const TextGridPresentationProvider& buildTextGridPresentation);

}  // namespace playback_framebuffer_presenter
