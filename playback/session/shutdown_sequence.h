#pragma once

#include <functional>
#include <vector>

#include "core/native_wait_handle.h"

namespace playback_session {

// Coordinates a fixed owner-scoped set of asynchronously stopping resources.
// Readiness is a pure barrier: no participant is finalized until every
// participant reports that finalization cannot block.
class ShutdownSequence {
 public:
  struct Participant {
    std::function<void()> requestStop;
    std::function<bool()> stopReady;
    std::function<bool()> finishStop;
    std::function<std::vector<NativeWaitHandle>()> waitHandles;
  };

  explicit ShutdownSequence(std::vector<Participant> participants);

  bool requestStop();
  bool requested() const;
  bool ready() const;
  bool finish();
  bool finished() const;
  std::vector<NativeWaitHandle> waitHandles() const;

 private:
  std::vector<Participant> participants_;
  bool requested_ = false;
  bool finished_ = false;
};

}  // namespace playback_session
