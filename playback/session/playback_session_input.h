#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <string>

#include "consoleinput.h"
#include "playback/render/playback_frame_output.h"
#include "playback/overlay/playback_overlay.h"
#include "playback_session_state.h"

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

  int* progressBarX = nullptr;
  int* progressBarY = nullptr;
  int* progressBarWidth = nullptr;

  playback_frame_output::LogLineWriter timingSink;
};

struct PlaybackInputSignals {
  std::atomic<int>* overlayControlHover = nullptr;
  std::atomic<WindowThreadState>* windowThreadState = nullptr;
  std::atomic<bool>* windowForcePresent = nullptr;
  std::condition_variable* windowPresentCv = nullptr;
  std::atomic<int64_t>* overlayUntilMs = nullptr;

  bool* windowEnabled = nullptr;
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
void signalWindowPresenterStop(const PlaybackInputSignals& signals);
void queueSeekRequest(PlaybackInputSignals& signals,
                      PlaybackSeekState& seekState, double targetSec);
void sendSeekRequest(const PlaybackInputView& view,
                     PlaybackInputSignals& signals,
                     PlaybackSeekState& seekState, double targetSec);

void handlePlaybackKeyEvent(const PlaybackInputView& view,
                            PlaybackInputSignals& signals,
                            PlaybackSeekState& seekState, const KeyEvent& key);
void handlePlaybackMouseEvent(const PlaybackInputView& view,
                              PlaybackInputSignals& signals,
                              PlaybackSeekState& seekState,
                              const MouseEvent& mouse);

}  // namespace playback_session_input
