#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "consolescreen.h"
#include "gpu_shared.h"
#include "player.h"
#include "playback_frame_output.h"
#include "playback_mode.h"
#include "playback_overlay.h"
#include "playback_session_state.h"
#include "subtitle_manager.h"
#include "videowindow.h"

namespace playback_screen_renderer {

struct PlaybackScreenRenderInputs {
  ConsoleScreen* screen = nullptr;
  VideoWindow* videoWindow = nullptr;
  Player* player = nullptr;
  SubtitleManager* subtitleManager = nullptr;
  GpuAsciiRenderer* gpuRenderer = nullptr;
  GpuVideoFrameCache* frameCache = nullptr;
  AsciiArt* art = nullptr;
  VideoFrame* frame = nullptr;
  const std::string* windowTitle = nullptr;
  const Style* baseStyle = nullptr;
  const Style* accentStyle = nullptr;
  const Style* dimStyle = nullptr;
  const Style* progressEmptyStyle = nullptr;
  const Style* progressFrameStyle = nullptr;
  const Color* progressStart = nullptr;
  const Color* progressEnd = nullptr;
  bool debugOverlay = false;
  std::vector<std::string> debugLines;
  PlaybackRenderMode currentMode = PlaybackRenderMode::Other;
  PlaybackSessionState playbackState = PlaybackSessionState::Active;
  bool enableAudio = false;
  bool audioOk = false;
  bool audioStarting = false;
  bool canPlayPrevious = false;
  bool canPlayNext = false;
  bool windowActive = false;
  bool hasSubtitles = false;
  bool allowAsciiCpuFallback = false;
  bool useWindowPresenter = false;
  bool overlayVisibleNow = false;
  bool clearHistory = false;
  bool frameChanged = false;
  bool frameAvailable = false;
  bool localSeekRequested = false;
  double cellPixelWidth = 0.0;
  double cellPixelHeight = 0.0;
  std::string cellPixelSourceLabel;
  double pendingSeekTargetSec = -1.0;
  std::atomic<bool>* enableSubtitlesShared = nullptr;
  std::atomic<bool>* windowLocalSeekRequested = nullptr;
  std::atomic<int>* overlayControlHover = nullptr;
  playback_frame_output::FrameOutputState* frameOutputState = nullptr;
  playback_frame_output::LogLineWriter warningSink;
  playback_frame_output::LogLineWriter timingSink;
};

void renderPlaybackScreen(PlaybackScreenRenderInputs& inputs);

}  // namespace playback_screen_renderer
