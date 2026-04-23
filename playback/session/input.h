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
#include "consoleinput.h"
#include "playback/ascii/frame_output.h"
#include "playback/overlay/overlay.h"
#include "playback_mode.h"
#include "state.h"

class Player;
class SubtitleManager;
class VideoWindow;

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

  playback_frame_output::LogLineWriter timingSink;
};

struct PlaybackInputSignals {
  std::atomic<int>* overlayControlHover = nullptr;
  std::function<void()> requestWindowPresent;
  std::function<bool()> toggleWindowPresentation;
  std::function<bool()> togglePictureInPicture;
  std::function<bool()> toggleFullscreen;
  std::function<void()> closePresentation;
  std::function<bool(PlaybackTransportCommand)> requestTransportCommand;
  std::function<bool(const std::vector<std::filesystem::path>&)> requestOpenFiles;
  std::atomic<int64_t>* overlayUntilMs = nullptr;

  bool* loopStopRequested = nullptr;
  bool* quitApplicationRequested = nullptr;
  bool* redraw = nullptr;
  bool* forceRefreshArt = nullptr;
};

struct PlaybackSeekState {
  bool* localSeekRequested = nullptr;
  std::atomic<bool>* windowLocalSeekRequested = nullptr;
  double* pendingSeekTargetSec = nullptr;
  std::atomic<double>* windowPendingSeekTargetSec = nullptr;
  std::chrono::steady_clock::time_point* seekRequestTime = nullptr;
  std::chrono::steady_clock::time_point* lastSeekSentTime = nullptr;
  double* queuedSeekTargetSec = nullptr;
  bool* seekQueued = nullptr;
};

bool isOverlayVisible(const PlaybackInputSignals& signals);
void queueSeekRequest(PlaybackInputSignals& signals,
                      PlaybackSeekState& seekState, double targetSec);
void sendSeekRequest(const PlaybackInputView& view,
                     PlaybackInputSignals& signals,
                     PlaybackSeekState& seekState, double targetSec);

void handlePlaybackKeyEvent(const PlaybackInputView& view,
                            PlaybackInputSignals& signals,
                            PlaybackSeekState& seekState, const KeyEvent& key);
void handlePlaybackControlCommand(const PlaybackInputView& view,
                                  PlaybackInputSignals& signals,
                                  PlaybackSeekState& seekState,
                                  PlaybackControlCommand command);
void handlePlaybackMouseEvent(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekState& seekState,
                              const MouseEvent& mouse);

}  // namespace playback_session_input
