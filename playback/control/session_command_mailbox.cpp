#include "playback/control/session_command_mailbox.h"

void PlaybackControlSessionCommandMailbox::activate(
    PlaybackControlSessionId session) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!session.valid()) {
    activeSession_ = {};
    pending_.clear();
    return;
  }
  if (session != activeSession_) {
    activeSession_ = session;
    pending_.clear();
  }
}

void PlaybackControlSessionCommandMailbox::deactivate() {
  std::lock_guard<std::mutex> lock(mutex_);
  activeSession_ = {};
  pending_.clear();
}

void PlaybackControlSessionCommandMailbox::publish(
    PlaybackControlCommand command) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!activeSession_.valid()) return;
  pending_.push_back({activeSession_, command});
}

bool PlaybackControlSessionCommandMailbox::poll(
    PlaybackControlCommandEvent* out) {
  if (!out) return false;
  std::lock_guard<std::mutex> lock(mutex_);
  while (!pending_.empty() &&
         pending_.front().session != activeSession_) {
    pending_.pop_front();
  }
  if (pending_.empty()) return false;
  *out = pending_.front();
  pending_.pop_front();
  return true;
}
