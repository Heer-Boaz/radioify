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
#include "playback/overlay/overlay.h"
#include "playback/session/presentation_policy.h"
#include "playback/session/state.h"
#include "playback/video/subtitle/manager.h"
#include "playback/video/framebuffer/window/window.h"

class AudioPlaybackRuntime;

namespace playback_screen_renderer {

struct TimelinePreviewAsciiCache {
  uint64_t imageId = 0;
  int width = 0;
  int height = 0;
  AsciiArt art;
  AsciiArtRenderer renderer;
};

struct PlaybackScreenResources {
  Player& player;
  const AudioPlaybackRuntime& audioPlayback;
  SubtitleManager& subtitleManager;
  GpuAsciiRenderer& gpuRenderer;
  const std::string& windowTitle;
  const Style& baseStyle;
  const Style& accentStyle;
  const Style& dimStyle;
  const Style& progressEmptyStyle;
  const Style& progressFrameStyle;
  const Color& progressStart;
  const Color& progressEnd;
  std::atomic<bool>& subtitlesEnabled;
  std::atomic<int>& controlHover;
  playback_frame_output::LogLineWriter warningSink;
  playback_frame_output::LogLineWriter timingSink;
};

// Mutable caches belong to one concrete presentation surface and are never
// shared between the terminal and the native-window presenter.
struct PlaybackScreenTarget {
  ConsoleScreen& screen;
  VideoWindow& videoWindow;
  GpuVideoFrameCache& frameCache;
  AsciiArt& art;
  TimelinePreviewAsciiCache& timelinePreviewCache;
  VideoFrame& frame;
  playback_frame_output::FrameOutputState& frameOutput;
};

// Copyable, immutable-by-convention read model published by the playback
// loop. It contains no borrowed pointers and no renderer-owned cache state.
struct PlaybackScreenModel {
  bool debugOverlay = false;
  std::vector<std::string> debugLines;
  PlaybackVisualMode visualMode = PlaybackVisualMode::Framebuffer;
  PlaybackSessionState playbackState = PlaybackSessionState::Active;
  bool enableAudio = false;
  bool audioOk = false;
  bool audioStarting = false;
  bool canPlayPrevious = false;
  bool canPlayNext = false;
  bool nativeWindowActive = false;
  bool hasSubtitles = false;
  bool allowAsciiCpuFallback = false;
  playback_overlay::PlaybackOsdSnapshot osd;
  playback_overlay::ContextMenuSnapshot contextMenu;
  playback_video_timeline_preview::Snapshot timelinePreview;
  playback_video_edit::EditSnapshot videoEdit;
  playback_video_edit::ExportProgress videoEditExport;
  playback_video_edit::Prompt videoEditPrompt =
      playback_video_edit::Prompt::None;
  bool clearHistory = false;
  bool frameChanged = false;
  bool frameAvailable = false;
  double cellPixelWidth = 0.0;
  double cellPixelHeight = 0.0;
  std::string cellPixelSourceLabel;
};

void renderPlaybackScreen(const PlaybackScreenResources& resources,
                          PlaybackScreenTarget& target,
                          const PlaybackScreenModel& model);

}  // namespace playback_screen_renderer
