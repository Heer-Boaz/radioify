#pragma once

#include <memory>

#include "core/native_wait_handle.h"
#include "input_transport.h"
#include "presentation_policy.h"
#include "state.h"

class ConsoleScreen;
class Player;
struct PerfLog;

namespace playback_screen_renderer {
struct PlaybackScreenRenderInputs;
}

struct PlaybackSessionRefreshResult {
  bool framePresented = false;
  bool stateChanged = false;
};

class PlaybackSessionCore final : public playback_session_input::Transport {
 public:
  struct Args {
    Player& player;
    PerfLog& perfLog;
    bool enableAudio;
    bool enableAscii;
  };

  explicit PlaybackSessionCore(Args args);
  ~PlaybackSessionCore();

  PlaybackSessionCore(PlaybackSessionCore&&) noexcept;
  PlaybackSessionCore& operator=(PlaybackSessionCore&&) noexcept;

  PlaybackSessionCore(const PlaybackSessionCore&) = delete;
  PlaybackSessionCore& operator=(const PlaybackSessionCore&) = delete;

  void initialize(ConsoleScreen& screen);
  void bindRenderInputs(
      playback_screen_renderer::PlaybackScreenRenderInputs& renderInputs);
  void updateRenderInputs(
      playback_screen_renderer::PlaybackScreenRenderInputs& renderInputs) const;

  bool finalizeAudioStart();
  playback_session_input::TransportSnapshot snapshot() const override;
  bool seekTo(int64_t targetUs) override;
  bool seekBy(int64_t deltaUs) override;
  void setPaused(bool paused) override;
  bool requestFrameStep(
      playback_video_frame_step::Direction direction) override;
  bool cycleAudioTrack() override;
  void beginExit();
  bool applyPresentationSync(bool switchedAwayFromWindow);
  PlaybackSessionRefreshResult refresh(bool nativeWindowActive, bool& redraw);
  void setAsciiPresentation(ConsoleScreen& screen, bool enabled);
  uint64_t videoFrameCounter() const;
  bool waitForVideoFrame(uint64_t lastCounter, int timeoutMs) const;
  NativeWaitHandle videoFrameWaitHandle() const;
  void markPendingResize();
  void handlePendingResize(ConsoleScreen& screen,
                           PlaybackVisualMode visualMode, bool& redraw);
  void shutdownPlayer();
  void shutdownAudio();
  void shutdown();

  Player& player();
  const Player& player() const;
  PlaybackSessionState playbackState() const;
  bool audioOk() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
