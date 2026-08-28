#pragma once

#include <optional>
#include <utility>

#include "playback/session/exit_coordinator.h"

namespace tui_media_handoff {

// Owns the shell side of an external playback handoff. A command may wait for
// a playback-owned modal to close before the session can allocate a request id;
// once an id exists, only the matching session response may consume it.
template <typename Command>
class DeferredCommand {
 public:
  bool empty() const { return !command_; }
  bool awaitingRequest() const { return command_ && !requestId_; }
  bool requestPending() const { return command_ && requestId_; }

  bool enqueue(Command command) {
    if (command_) return false;
    command_.emplace(std::move(command));
    requestId_.reset();
    return true;
  }

  bool markRequestStarted(playback_session_exit::RequestId requestId) {
    if (!awaitingRequest() || requestId == 0) return false;
    requestId_ = requestId;
    return true;
  }

  bool matches(playback_session_exit::RequestId requestId) const {
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

}  // namespace tui_media_handoff
