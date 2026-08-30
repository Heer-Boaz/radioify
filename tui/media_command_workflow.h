#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <variant>

#include "app/playback_queue.h"
#include "playback/session/handoff_endpoint.h"
#include "tui/media_activation_plan.h"

namespace tui_media_command {

struct PreparedPlayback {
  playback_queue::Queue::PreparedActivation activation;
};

struct QuitApplication {};

using Command =
    std::variant<PreparedPlayback, tui_media_activation::ShowImages,
                 tui_media_activation::OpenDirectory, QuitApplication>;

enum class DeferralReason : std::uint8_t {
  AfterCurrentDispatch,
  InteractivePlayback,
  VideoSessionExit,
};

enum class HandoffProgress : std::uint8_t {
  Busy,
  NotAwaitingRequest,
  WaitingForInteraction,
  RequestStarted,
  RequestRejected,
  ProtocolFault,
};

enum class HandoffRequestResolution : std::uint8_t {
  Accepted,
  Declined,
  AbortedAfterAcknowledgementFailure,
  ProtocolFault,
};

// Owns the mutually exclusive owner-thread phases of shell media commands.
// A command is either being dispatched, explicitly deferred, or negotiating
// one video handoff; the same command cannot occupy more than one phase.
class Workflow {
 public:
  // A lease must not outlive its Workflow. Its destructor is the transaction
  // boundary that either publishes the queued command as deferred work or
  // returns the workflow to idle.
  class DispatchLease {
   public:
    ~DispatchLease();

    DispatchLease(DispatchLease&& other) noexcept;
    DispatchLease& operator=(DispatchLease&& other) noexcept;

    DispatchLease(const DispatchLease&) = delete;
    DispatchLease& operator=(const DispatchLease&) = delete;

   private:
    explicit DispatchLease(Workflow& owner) : owner_(&owner) {}
    void finish() noexcept;

    Workflow* owner_ = nullptr;

    friend class Workflow;
  };

  Workflow();
  ~Workflow();

  Workflow(const Workflow&) = delete;
  Workflow& operator=(const Workflow&) = delete;

  [[nodiscard]] bool idle() const noexcept;
  [[nodiscard]] bool dispatching() const noexcept;
  [[nodiscard]] bool hasDeferredCommand() const noexcept;
  [[nodiscard]] std::optional<DeferralReason> deferredReason() const noexcept;
  [[nodiscard]] bool handoffActive() const noexcept;
  [[nodiscard]] bool handoffAwaitingRequest() const noexcept;
  [[nodiscard]] bool handoffFaulted() const noexcept;

  [[nodiscard]] std::optional<DispatchLease> beginDispatch();
  [[nodiscard]] bool queueDuringDispatch(Command command,
                                         DeferralReason reason);
  [[nodiscard]] std::optional<Command> takeDuringDispatch();
  void discardQueuedDuringDispatch() noexcept;

  [[nodiscard]] std::optional<Command> takeDeferred();

  [[nodiscard]] HandoffProgress beginHandoff(
      Command command, playback_session::HandoffEndpoint& endpoint);
  [[nodiscard]] HandoffProgress resumeHandoff(
      playback_session::HandoffEndpoint& endpoint);
  [[nodiscard]] HandoffRequestResolution resolveExternalHandoffRequest(
      playback_session_exit::RequestId requestId,
      playback_session::HandoffEndpoint& endpoint);
  [[nodiscard]] HandoffRequestResolution resolveSessionHandoffRequest(
      Command command, playback_session_exit::RequestId requestId,
      playback_session::HandoffEndpoint& endpoint);
  [[nodiscard]] HandoffRequestResolution declineHandoffRequest(
      playback_session_exit::RequestId requestId,
      playback_session::HandoffEndpoint& endpoint);
  [[nodiscard]] bool cancelHandoff(
      playback_session_exit::RequestId requestId) noexcept;
  [[nodiscard]] bool releaseHandoffForSessionExit();

  // This is a lifecycle-bound transition: the caller has already destroyed
  // the video endpoint, so any unresolved request identity has ended with the
  // session. An older media change must never replace the committed quit.
  void preemptWithQuitAfterSessionExit();
  // The caller must likewise have closed the handoff endpoint. An active
  // dispatch cannot be interrupted; only work retained beyond it is dropped.
  void discardRetainedCommands() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  [[nodiscard]] bool matchesHandoff(
      playback_session_exit::RequestId requestId) const noexcept;
  [[nodiscard]] HandoffRequestResolution recoverRejectedAcknowledgement(
      playback_session_exit::RequestId requestId,
      playback_session::HandoffEndpoint& endpoint,
      bool discardMatchingCommand);
  void finishDispatch() noexcept;

  friend class DispatchLease;
};

}  // namespace tui_media_command
