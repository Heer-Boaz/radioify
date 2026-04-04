#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>

#include "playback_mode.h"
#include "playback_session_state.h"

class ConsoleScreen;
class Player;
struct PerfLog;
struct PlaybackPresenterSyncResult;

namespace playback_session_input {
struct PlaybackInputView;
struct PlaybackSeekState;
}

namespace playback_screen_renderer {
struct PlaybackScreenRenderInputs;
}

class PlaybackSessionCore {
 public:
  struct Args {
    Player& player;
    PerfLog& perfLog;
    bool enableAudio = true;
    bool enableAscii = true;
  };

  explicit PlaybackSessionCore(Args args);
  ~PlaybackSessionCore();

  PlaybackSessionCore(PlaybackSessionCore&&) noexcept;
  PlaybackSessionCore& operator=(PlaybackSessionCore&&) noexcept;

  PlaybackSessionCore(const PlaybackSessionCore&) = delete;
  PlaybackSessionCore& operator=(const PlaybackSessionCore&) = delete;

  void initialize(ConsoleScreen& screen);
  void bindInputView(playback_session_input::PlaybackInputView& inputView);
  void bindSeekState(playback_session_input::PlaybackSeekState& seekState);
  void bindRenderInputs(
      playback_screen_renderer::PlaybackScreenRenderInputs& renderInputs);
  void updateRenderInputs(
      playback_screen_renderer::PlaybackScreenRenderInputs& renderInputs) const;

  bool finalizeAudioStart();
  void applyPresenterSync(const PlaybackPresenterSyncResult& syncResult);
  bool refresh(bool useWindowPresenter, bool windowActive, bool& redraw);
  uint64_t videoFrameCounter() const;
  bool waitForVideoFrame(uint64_t lastCounter, int timeoutMs) const;
  HANDLE videoFrameWaitHandle() const;
  void markPendingResize();
  void handlePendingResize(ConsoleScreen& screen, PlaybackRenderMode renderMode,
                           bool& redraw);
  void shutdown();

  Player& player();
  const Player& player() const;
  PlaybackSessionState playbackState() const;
  bool audioOk() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
