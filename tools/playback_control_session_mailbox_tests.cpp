#include <cstdio>

#include "playback/control/session_command_mailbox.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::fprintf(stderr, "playback_control_session_mailbox_tests: %s\n",
               message);
  return false;
}

}  // namespace

int main() {
  bool ok = true;
  PlaybackControlSessionCommandMailbox mailbox;
  PlaybackControlCommandEvent event;

  mailbox.publish(PlaybackControlCommand::Pause);
  ok &= expect(!mailbox.poll(&event),
               "commands without an active session must be ignored");

  const PlaybackControlSessionId first{1};
  const PlaybackControlSessionId second{2};
  mailbox.activate(first);
  mailbox.publish(PlaybackControlCommand::Pause);
  mailbox.publish(PlaybackControlCommand::Next);
  ok &= expect(mailbox.poll(&event) && event.session == first &&
                   event.command == PlaybackControlCommand::Pause,
               "the active session must receive commands in FIFO order");
  ok &= expect(mailbox.poll(&event) && event.session == first &&
                   event.command == PlaybackControlCommand::Next,
               "the active session must retain the remaining FIFO order");

  mailbox.publish(PlaybackControlCommand::Stop);
  mailbox.activate(second);
  ok &= expect(!mailbox.poll(&event),
               "starting another session must invalidate queued commands");

  mailbox.publish(PlaybackControlCommand::Play);
  mailbox.deactivate();
  ok &= expect(!mailbox.poll(&event),
               "ending playback must invalidate queued commands");
  mailbox.publish(PlaybackControlCommand::Pause);
  ok &= expect(!mailbox.poll(&event),
               "commands received after playback ends must be ignored");

  mailbox.activate(second);
  mailbox.publish(PlaybackControlCommand::TogglePause);
  mailbox.activate(second);
  ok &= expect(mailbox.poll(&event) && event.session == second &&
                   event.command == PlaybackControlCommand::TogglePause,
               "refreshing the same session must preserve queued commands");

  return ok ? 0 : 1;
}
