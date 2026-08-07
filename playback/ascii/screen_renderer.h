#pragma once

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "consolescreen.h"
#include "playback/video/gpu/gpu_shared.h"
#include "playback/video/player.h"
#include "frame_output.h"
#include "playback_mode.h"
#include "playback/overlay/overlay.h"
#include "playback/session/state.h"
#include "playback/video/subtitle/manager.h"
#include "playback/video/framebuffer/window/window.h"

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
  playback_overlay::PlaybackOsdSnapshot osd;
  bool clearHistory = false;
  bool frameChanged = false;
  bool frameAvailable = false;
  double cellPixelWidth = 0.0;
  double cellPixelHeight = 0.0;
  std::string cellPixelSourceLabel;
  std::atomic<bool>* enableSubtitlesShared = nullptr;
  std::atomic<int>* overlayControlHover = nullptr;
  playback_frame_output::FrameOutputState* frameOutputState = nullptr;
  playback_frame_output::LogLineWriter warningSink;
  playback_frame_output::LogLineWriter timingSink;
};

void renderPlaybackScreen(PlaybackScreenRenderInputs& inputs);

}  // namespace playback_screen_renderer
