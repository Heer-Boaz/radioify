#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "asciiart.h"
#include "asciiart_gpu.h"
#include "consolescreen.h"
#include "playback/video/gpu/gpu_runtime.h"
#include "playback/video/player.h"
#include "frame_output.h"
#include "playback/overlay/overlay.h"
#include "playback/session/presentation_policy.h"
#include "playback/session/state.h"

namespace playback_screen_renderer {

struct TimelinePreviewAsciiCache {
  uint64_t imageId = 0;
  int width = 0;
  int height = 0;
  AsciiArt art;
  AsciiArtRenderer renderer;
};

struct PlaybackScreenResources {
  GpuRuntime& gpu;
  const Style& baseStyle;
  const Style& accentStyle;
  const Style& dimStyle;
  const Style& progressEmptyStyle;
  const Style& progressFrameStyle;
  const Color& progressStart;
  const Color& progressEnd;
  playback_frame_output::LogLineWriter warningSink;
  playback_frame_output::LogLineWriter timingSink;
};

// Mutable caches belong to one concrete presentation surface and are never
// shared between the terminal and the native-window presenter.
struct PlaybackScreenTarget {
  ConsoleScreen& screen;
  GpuVideoFrameCache& frameCache;
  AsciiArt& art;
  TimelinePreviewAsciiCache& timelinePreviewCache;
  VideoFrame& frame;
  playback_frame_output::FrameOutputState& frameOutput;
};

struct PlaybackAudioPresentation {
  bool streamClockReady = false;
  bool streamStarved = false;
  bool finished = false;
  bool supports50HzToggle = false;
  bool radioEnabled = false;
  bool hz50Enabled = false;
  double durationSec = -1.0;
  float volume = 0.0f;
  std::string radioFilterLabel = "Radio: Off";
};

// Complete, owned projection of mutable playback services for one rendered
// revision. Render threads never reach back into Player, audio, or subtitles.
struct PlaybackMediaPresentation {
  std::string windowTitle;
  PlayerTimelineSnapshot timeline;
  PlayerDebugInfo debug;
  PlaybackAudioPresentation audio;
  playback_overlay::SubtitlePresentation subtitle;
  int64_t durationUs = 0;
  int sourceWidth = 0;
  int sourceHeight = 0;
  std::size_t audioTrackCount = 0;
  bool ended = false;
  bool canCycleAudioTracks = false;
  bool hasSubtitles = false;
  bool subtitlesEnabled = false;
  std::string activeAudioTrackLabel = "N/A";
};

// Copyable, immutable-by-convention read model published by the playback
// loop. It contains no borrowed pointers and no renderer-owned cache state.
struct PlaybackScreenModel {
  PlaybackMediaPresentation media;
  playback_overlay::PlaybackOverlayState overlay;
  bool debugOverlay = false;
  PlaybackVisualMode visualMode = PlaybackVisualMode::Framebuffer;
  PlaybackSessionState playbackState = PlaybackSessionState::Active;
  bool enableAudio = false;
  bool audioOk = false;
  bool audioStarting = false;
  bool nativeWindowActive = false;
  bool allowAsciiCpuFallback = false;
  playback_video_timeline_preview::Snapshot timelinePreview;
  int controlHoverToken = -1;
  int nativeWindowWidth = 0;
  int nativeWindowHeight = 0;
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
