#include <cstdio>
#include <thread>

#include <windows.h>

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
  const HANDLE ready =
      static_cast<HANDLE>(mailbox.nativeWaitHandle().get());

  ok &= expect(ready != nullptr,
               "the command mailbox must expose a waitable ingress");
  ok &= expect(WaitForSingleObject(ready, 0) == WAIT_TIMEOUT,
               "an empty mailbox must not wake the application loop");

  mailbox.publish(PlaybackControlCommand::Pause);
  ok &= expect(!mailbox.poll(&event),
               "commands without an active session must be ignored");
  ok &= expect(WaitForSingleObject(ready, 0) == WAIT_TIMEOUT,
               "an ignored command must leave the mailbox asleep");

  const PlaybackControlSessionId first{1};
  const PlaybackControlSessionId second{2};
  mailbox.activate(first);
  mailbox.publish(PlaybackControlCommand::Pause);
  mailbox.publish(PlaybackControlCommand::Next);
  ok &= expect(WaitForSingleObject(ready, 0) == WAIT_OBJECT_0,
               "publishing work must wake the application loop");
  ok &= expect(mailbox.poll(&event) && event.session == first &&
                   event.command == PlaybackControlCommand::Pause,
               "the active session must receive commands in FIFO order");
  ok &= expect(WaitForSingleObject(ready, 0) == WAIT_OBJECT_0,
               "the mailbox must stay awake while another command remains");
  ok &= expect(mailbox.poll(&event) && event.session == first &&
                   event.command == PlaybackControlCommand::Next,
               "the active session must retain the remaining FIFO order");
  ok &= expect(WaitForSingleObject(ready, 0) == WAIT_TIMEOUT,
               "draining the mailbox must reset its wake signal");

  mailbox.publish(PlaybackControlCommand::Stop);
  mailbox.activate(second);
  ok &= expect(!mailbox.poll(&event),
               "starting another session must invalidate queued commands");
  ok &= expect(WaitForSingleObject(ready, 0) == WAIT_TIMEOUT,
               "session replacement must clear the stale wake signal");

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

  std::thread callbackThread([&]() {
    mailbox.publish(PlaybackControlCommand::Pause);
  });
  const DWORD callbackWake = WaitForSingleObject(ready, 2000);
  callbackThread.join();
  ok &= expect(callbackWake == WAIT_OBJECT_0,
               "a command published by an OS callback thread must wake the "
               "application loop");
  ok &= expect(mailbox.poll(&event) && event.session == second &&
                   event.command == PlaybackControlCommand::Pause,
               "the woken loop must drain the callback command for the "
               "active session");
  ok &= expect(WaitForSingleObject(ready, 0) == WAIT_TIMEOUT,
               "the callback wake must reset after its command is drained");

  return ok ? 0 : 1;
}
