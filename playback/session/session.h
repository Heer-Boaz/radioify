#pragma once

#include <memory>
#include "playback/session/video_session.h"
#include "tui/style.h"

class ConsoleScreen;
class AudioPlaybackRuntime;
class GpuRuntime;
struct InputEvent;
namespace playback_session {
class SubtitleLoadService;
}

class PlaybackSession final : public playback_session::VideoSession {
 public:
  struct Appearance {
    Style baseStyle;
    Style accentStyle;
    Style dimStyle;
    Style progressEmptyStyle;
    Style progressFrameStyle;
    Color progressStart;
    Color progressEnd;
  };

  // Process services and the screen are borrowed and must outlive the
  // session. Appearance is copied so presentation state has no separate
  // lifetime contract.
  struct Dependencies {
    AudioPlaybackRuntime& audioPlayback;
    GpuRuntime& gpu;
    ConsoleScreen& screen;
    playback_session::SubtitleLoadService& subtitleLoader;
    Appearance appearance;
  };

  PlaybackSession(playback_session::VideoSessionRequest request,
                  Dependencies dependencies);
  ~PlaybackSession() override;

  PlaybackSession(PlaybackSession&&) noexcept;
  PlaybackSession& operator=(PlaybackSession&&) noexcept;

  PlaybackSession(const PlaybackSession&) = delete;
  PlaybackSession& operator=(const PlaybackSession&) = delete;

  // Opening can be asynchronous. An empty startOpen() result keeps the session
  // alive but not controllable; the owner drives pumpOpen(), wait handles and
  // deadlines until a terminal open outcome is returned.
  std::optional<playback_session::OpenOutcome> startOpen() override;
  std::optional<playback_session::OpenOutcome> pumpOpen() override;
  bool opening() const override;
  bool ready() const override;
  std::optional<playback_session::TransitionSnapshot> transitionSnapshot()
      const override;
  std::optional<PlaybackSessionCompletion> pump() override;
  PlaybackShellTerminalRole terminalRole() const override;
  std::vector<NativeWaitHandle> activityWaitHandles() const override;
  wake_schedule::Deadline nextWakeDeadline() const override;
  std::optional<playback_session::ViewSnapshot> viewSnapshot()
      const override;
  bool capturesBrowserInput() const override;
  void setExternalInputModal(bool modal) override;
  bool handleInputEvent(const InputEvent& event) override;
  bool pollWindowInput(InputEvent& event) override;
  bool handleWindowInputEvent(const InputEvent& event) override;
  bool handleControlCommand(PlaybackControlCommand command) override;
  bool seekToRatio(double ratio) override;
  bool toggleWindowPresentation() override;
  bool togglePictureInPicture() override;
  bool toggleFullscreen() override;
  bool activatePresentation() override;
  std::optional<playback_session_exit::RequestId> requestHandoff() override;
  bool resolveHandoff(playback_session_exit::RequestId requestId,
                      bool accepted) override;
  bool abortHandoff(
      playback_session_exit::RequestId requestId) override;
  std::vector<playback_session::Event> drainEvents() override;
  void mediaTaskFinished(
      const playback_media_processing::Completion& completion) override;
  void mediaTaskActivityChanged(
      std::optional<playback_media_processing::Activity> activity) override;
  void requestStop() override;
  void requestQuit() override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
