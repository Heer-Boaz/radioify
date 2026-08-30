#pragma once

#include <cstdint>
#include <optional>
#include <utility>

#include "playback/session/handoff_endpoint.h"

namespace application_playback {

enum class VideoHandoffStart : std::uint8_t {
  NoPendingCommand,
  WaitingForInteraction,
  RequestStarted,
  Failed,
};

// Owns one deferred application command while the active video session
// negotiates its exit. No endpoint pointer is retained: the session owner
// supplies a live handoff port for each owner-thread transition.
template <typename Command>
class VideoHandoffController {
 public:
  [[nodiscard]] bool empty() const { return !command_; }
  [[nodiscard]] bool awaitingRequest() const {
    return command_ && !requestId_;
  }
  [[nodiscard]] bool requestPending() const {
    return command_ && requestId_;
  }

  bool enqueue(Command command) {
    if (command_) return false;
    command_.emplace(std::move(command));
    requestId_.reset();
    return true;
  }

  VideoHandoffStart tryStart(playback_session::HandoffEndpoint& endpoint) {
    if (!awaitingRequest()) {
      return VideoHandoffStart::NoPendingCommand;
    }
    if (endpoint.handoffRequestDeferred()) {
      return VideoHandoffStart::WaitingForInteraction;
    }

    const std::optional<playback_session_exit::RequestId> requestId =
        endpoint.requestHandoff();
    if (!requestId) {
      clear();
      return VideoHandoffStart::Failed;
    }
    if (*requestId == 0) {
      (void)endpoint.resolveHandoff(*requestId, false);
      clear();
      return VideoHandoffStart::Failed;
    }
    requestId_ = *requestId;
    return VideoHandoffStart::RequestStarted;
  }

  [[nodiscard]] bool matches(
      playback_session_exit::RequestId requestId) const {
    return requestPending() && requestId_ == requestId;
  }

  std::optional<Command> accept(
      playback_session_exit::RequestId requestId) {
    if (!matches(requestId)) return std::nullopt;
    return release();
  }

  bool cancel(playback_session_exit::RequestId requestId) {
    if (!matches(requestId)) return false;
    clear();
    return true;
  }

  std::optional<Command> release() {
    std::optional<Command> command = std::move(command_);
    clear();
    return command;
  }

  void clear() {
    command_.reset();
    requestId_.reset();
  }

 private:
  std::optional<Command> command_;
  std::optional<playback_session_exit::RequestId> requestId_;
};

}  // namespace application_playback
