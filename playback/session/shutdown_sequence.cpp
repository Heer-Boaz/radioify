#include "shutdown_sequence.h"

#include <algorithm>
#include <utility>

namespace playback_session {

ShutdownSequence::ShutdownSequence(std::vector<Participant> participants)
    : participants_(std::move(participants)) {}

bool ShutdownSequence::requestStop() {
  if (requested_) return false;
  requested_ = true;
  for (const Participant& participant : participants_) {
    if (participant.requestStop) participant.requestStop();
  }
  return true;
}

bool ShutdownSequence::requested() const { return requested_; }

bool ShutdownSequence::ready() const {
  return requested_ && std::all_of(participants_.begin(), participants_.end(),
                                   [](const Participant& participant) {
                                     return !participant.stopReady ||
                                            participant.stopReady();
                                   });
}

bool ShutdownSequence::finish() {
  if (finished_) return true;
  if (!ready()) return false;
  for (const Participant& participant : participants_) {
    if (participant.finishStop && !participant.finishStop()) return false;
  }
  finished_ = true;
  return true;
}

bool ShutdownSequence::finished() const { return finished_; }

std::vector<NativeWaitHandle> ShutdownSequence::waitHandles() const {
  std::vector<NativeWaitHandle> handles;
  if (!requested_ || finished_) return handles;
  for (const Participant& participant : participants_) {
    if (participant.stopReady && participant.stopReady()) continue;
    if (!participant.waitHandles) continue;
    for (const NativeWaitHandle handle : participant.waitHandles()) {
      if (handle) handles.push_back(handle);
    }
  }
  return handles;
}

}  // namespace playback_session
