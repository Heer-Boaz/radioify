#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "playback/control/command.h"
#include "playback/control/transport.h"
#include "playback/input/shortcut_types.h"
#include "consoleinput.h"
#include "playback/ascii/frame_output.h"
#include "playback/overlay/overlay.h"
#include "playback_mode.h"
#include "state.h"

class Player;
class SubtitleManager;
class VideoWindow;
namespace playback_session {
class PlaybackOsdTimeline;
}

namespace playback_session_input {

struct PlaybackInputView {
  Player* player = nullptr;
  ConsoleScreen* screen = nullptr;
  VideoWindow* videoWindow = nullptr;
  SubtitleManager* subtitleManager = nullptr;
  const std::string* windowTitle = nullptr;

  std::atomic<bool>* enableSubtitlesShared = nullptr;
  PlaybackSessionState* playbackState = nullptr;
  bool* audioOk = nullptr;
  bool hasSubtitles = false;
  PlaybackRenderMode currentMode = PlaybackRenderMode::Other;

  playback_frame_output::FrameOutputState* frameOutputState = nullptr;
  playback_frame_output::FrameOutputState* textGridPresentationOutputState =
      nullptr;
  const playback_video_edit::EditSnapshot* videoEdit = nullptr;
  const playback_video_edit::ExportProgress* videoEditExport = nullptr;

  playback_frame_output::LogLineWriter timingSink;
};

struct PlaybackInputSignals {
  std::atomic<int>* overlayControlHover = nullptr;
  std::function<void()> requestWindowPresent;
  std::function<bool()> toggleWindowPresentation;
  std::function<bool()> togglePictureInPicture;
  std::function<bool()> toggleFullscreen;
  std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
  std::function<bool(const std::vector<std::filesystem::path>&)> requestOpenFiles;
  std::function<void()> copyCurrentVideoFrameToClipboard;
  std::function<bool()> videoEditorActive;
  std::function<bool(PlaybackShortcutAction)> handleVideoEditorAction;
  std::function<void(playback_video_timeline_preview::PresentationSurface,
                     double ratio, int progressUnits)>
      requestTimelinePreview;
  std::function<void(playback_video_timeline_preview::PresentationSurface)>
      clearTimelinePreview;
  playback_session::PlaybackOsdTimeline* osd = nullptr;

  bool* loopStopRequested = nullptr;
  bool* quitApplicationRequested = nullptr;
  bool* redraw = nullptr;
  bool* forceRefreshArt = nullptr;
};

struct PlaybackSeekGestureState {
  std::chrono::steady_clock::time_point lastSeekSentTime =
      std::chrono::steady_clock::time_point::min();
  double queuedSeekTargetSec = -1.0;
  bool seekQueued = false;
};

bool isOverlayVisible(const PlaybackInputSignals& signals);
void setPlaybackPaused(const PlaybackInputView& view,
                       PlaybackInputSignals& signals,
                       PlaybackSeekGestureState& seekState, bool paused);
void queueSeekRequest(PlaybackInputSignals& signals,
                      PlaybackSeekGestureState& seekState, double targetSec);
void sendSeekRequest(const PlaybackInputView& view,
                     PlaybackInputSignals& signals,
                     PlaybackSeekGestureState& seekState, double targetSec);

void handlePlaybackInputEvent(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekGestureState& seekState,
                              const InputEvent& ev);
void handlePlaybackControlCommand(const PlaybackInputView& view,
                                  PlaybackInputSignals& signals,
                                  PlaybackSeekGestureState& seekState,
                                  PlaybackControlCommand command);
void handlePlaybackMouseEvent(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekGestureState& seekState,
                              const MouseEvent& mouse);
void handlePlaybackPointerLeave(PlaybackInputSignals& signals);

}  // namespace playback_session_input
