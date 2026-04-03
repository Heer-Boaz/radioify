#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

#include "gpu_shared.h"
#include "playback_overlay.h"
#include "playback_session_state.h"
#include "player.h"
#include "videowindow.h"

namespace playback_framebuffer_presenter {

WindowUiState buildPlaybackFramebufferUiState(
    const std::string& windowTitle, VideoWindow& videoWindow, Player& player,
    SubtitleManager& subtitleManager, PlaybackSessionState playbackState,
    bool audioOk, bool hasSubtitles,
    std::atomic<bool>& enableSubtitlesShared,
    std::atomic<bool>& windowLocalSeekRequested,
    std::atomic<double>& windowPendingSeekTargetSec,
    std::atomic<int>& overlayControlHover, bool overlayVisibleNow);

void runFramebufferPresenterLoop(
    Player& player, VideoWindow& videoWindow, GpuVideoFrameCache& frameCache,
    std::atomic<WindowThreadState>& threadState,
    std::atomic<bool>& forcePresent, std::mutex& presentMutex,
    std::condition_variable& presentCv, const std::function<bool()>& overlayVisible,
    const std::function<WindowUiState()>& buildUiState);

}  // namespace playback_framebuffer_presenter
