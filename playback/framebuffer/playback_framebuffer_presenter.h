#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "consolescreen.h"
#include "gpu_shared.h"
#include "playback_overlay.h"
#include "playback_session_state.h"
#include "player.h"
#include "videowindow.h"

namespace playback_framebuffer_presenter {

using TextGridPresentationProvider =
    std::function<bool(int pixelWidth, int pixelHeight, int cellPixelWidth,
                       int cellPixelHeight,
                       const VideoFrame* frame, bool frameChanged,
                       const std::string& enhancementDebugLine,
                       std::vector<ScreenCell>& outCells,
                       int& outCols, int& outRows)>;

WindowUiState buildPlaybackFramebufferUiState(
    const std::string& windowTitle, VideoWindow& videoWindow, Player& player,
    SubtitleManager& subtitleManager, PlaybackSessionState playbackState,
    bool audioOk, bool canPlayPrevious, bool canPlayNext, bool hasSubtitles,
    std::atomic<bool>& enableSubtitlesShared,
    std::atomic<bool>& windowLocalSeekRequested,
    std::atomic<double>& windowPendingSeekTargetSec,
    std::atomic<int>& overlayControlHover, bool overlayVisibleNow,
    bool debugOverlay);

void runFramebufferPresenterLoop(
    Player& player, VideoWindow& videoWindow, GpuVideoFrameCache& frameCache,
    std::atomic<WindowThreadState>& threadState,
    std::atomic<bool>& forcePresent, HANDLE wakeEvent,
    const std::function<bool()>& overlayVisible,
    const std::function<WindowUiState()>& buildUiState,
    const TextGridPresentationProvider& buildTextGridPresentation);

}  // namespace playback_framebuffer_presenter
