#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

#include "app/playback_activation_controller.h"
#include "app/playback_queue.h"
#include "playback/session/handoff_endpoint.h"
#include "playback/session/video_session.h"
#include "playback/target.h"

namespace application_playback {

enum class VideoSessionStartFailure : std::uint8_t {
  HostOccupied,
  FactoryReturnedNoSession,
};

// Owns the mutually exclusive lifecycle states of one video session. The
// caller retains policy decisions (queue commit, fallback and user-visible
// errors), while session, target, prepared activation and its suspended
// transport transaction transition as one unit and cannot drift apart.
class VideoSessionHost final : public playback_session::HandoffEndpoint {
 public:
  enum class Lifecycle : std::uint8_t {
    Empty,
    Opening,
    Active,
    Completed,
  };

  // Owner-thread projection of the host variant for shell routing. A
  // completed session is intentionally inert: completion has moved to the
  // host and its former endpoint is never queried again.
  struct Snapshot {
    Lifecycle lifecycle = Lifecycle::Empty;
    std::optional<PlaybackShellTerminalRole> terminalRole;
    std::optional<playback_session::TransitionSnapshot> transition;
    bool controllable = false;
    bool capturesBrowserInput = false;

    [[nodiscard]] bool empty() const noexcept {
      return lifecycle == Lifecycle::Empty;
    }
    [[nodiscard]] bool ready() const noexcept {
      return lifecycle == Lifecycle::Active && controllable;
    }
  };

  struct OpenPending {};
  struct OpenFinished {
    VideoActivationTransaction transaction;
    playback_queue::Queue::PreparedActivation activation;
    playback_session::OpenOutcome outcome;
  };
  struct StartRejected {
    VideoSessionStartFailure failure =
        VideoSessionStartFailure::HostOccupied;
  };
  struct SessionFinished {
    PlaybackTarget target;
    PlaybackSessionCompletion completion;
  };

  using StartResult =
      std::variant<OpenPending, OpenFinished, StartRejected>;
  using SessionRef =
      std::optional<std::reference_wrapper<playback_session::VideoSession>>;
  using ConstSessionRef = std::optional<
      std::reference_wrapper<const playback_session::VideoSession>>;

  explicit VideoSessionHost(
      playback_session::VideoSessionFactory factory);
  ~VideoSessionHost();

  VideoSessionHost(const VideoSessionHost&) = delete;
  VideoSessionHost& operator=(const VideoSessionHost&) = delete;

  [[nodiscard]] bool empty() const;
  [[nodiscard]] bool opening() const;
  [[nodiscard]] bool completionPending() const;
  [[nodiscard]] bool ready() const;

  // Only Opening and Active expose a live endpoint. Completed retains the
  // object for ordered destruction but is deliberately absent here.
  SessionRef session();
  ConstSessionRef session() const;

  [[nodiscard]] Snapshot snapshot() const;
  [[nodiscard]] std::optional<playback_session::ViewSnapshot> viewSnapshot()
      const;
  [[nodiscard]] std::vector<NativeWaitHandle> activityWaitHandles() const;
  [[nodiscard]] wake_schedule::Deadline nextWakeDeadline() const;

  void setExternalInputModal(bool modal);
  bool handleInputEvent(const InputEvent& event);
  bool pollWindowInput(InputEvent& event);
  bool handleWindowInputEvent(const InputEvent& event);
  bool toggleWindowPresentation();
  bool togglePictureInPicture();
  bool toggleFullscreen();
  bool activatePresentation();
  std::vector<playback_session::Event> drainEvents();
  void mediaTaskFinished(
      const playback_media_processing::Completion& completion);
  void mediaTaskActivityChanged(
      std::optional<playback_media_processing::Activity> activity);
  bool requestQuit();

  bool handoffRequestDeferred() const override;
  std::optional<playback_session_exit::RequestId> requestHandoff() override;
  bool resolveHandoff(playback_session_exit::RequestId requestId,
                      bool accepted) override;
  bool abortHandoff(
      playback_session_exit::RequestId requestId) override;

  StartResult start(
      playback_session::VideoSessionRequest request,
      VideoActivationTransaction transaction,
      playback_queue::Queue::PreparedActivation activation);
  std::optional<OpenFinished> pumpOpen();
  bool pumpPlayback();
  std::optional<SessionFinished> takeCompletion();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace application_playback
