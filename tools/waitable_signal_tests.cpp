#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <vector>

#include "core/wake_event.h"
#include "core/wakeable_mailbox.h"
#include "core/waitable_signal.h"

namespace {
bool expect(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    return false;
  }
  return true;
}

DWORD waitNow(const WaitableSignal& signal) {
  return WaitForSingleObject(
      static_cast<HANDLE>(signal.nativeWaitHandle().get()), 0);
}
}  // namespace

int main() {
  bool ok = true;
  WaitableSignal signal;

  ok &= expect(waitNow(signal) == WAIT_TIMEOUT,
               "new signal must start unsignaled");
  ok &= expect(!signal.consume(),
               "consume must report false for an unsignaled state");

  signal.signal();
  ok &= expect(waitNow(signal) == WAIT_OBJECT_0,
               "signal must wake native waiters");
  ok &= expect(signal.consume(), "consume must report a signaled state");
  ok &= expect(waitNow(signal) == WAIT_TIMEOUT,
               "consume must reset the native wait handle");
  ok &= expect(!signal.consume(),
               "consume must be single-shot after reset");

  signal.signal();
  signal.clear();
  ok &= expect(waitNow(signal) == WAIT_TIMEOUT,
               "clear must reset a signaled state");
  ok &= expect(!signal.consume(), "clear must clear the latched state");

  WakeNotifier expiredNotifier;
  {
    WakeEvent event;
    expiredNotifier = event.notifier();
    expiredNotifier.notify();
    ok &= expect(WaitForSingleObject(
                     static_cast<HANDLE>(event.nativeWaitHandle().get()), 0) ==
                     WAIT_OBJECT_0,
                 "a producer token must wake its owner event");
    ok &= expect(event.consume(),
                 "only the event owner must consume a fanned-in wake");
  }
  expiredNotifier.notify();

  WakeableMailbox<int> mailbox;
  ok &= expect(WaitForSingleObject(
                   static_cast<HANDLE>(mailbox.nativeWaitHandle().get()), 0) ==
                   WAIT_TIMEOUT,
               "a new mailbox must start unsignaled");
  mailbox.publish(17);
  mailbox.publish(23);
  ok &= expect(WaitForSingleObject(
                   static_cast<HANDLE>(mailbox.nativeWaitHandle().get()), 0) ==
                   WAIT_OBJECT_0,
               "publishing must wake the mailbox owner");
  const std::vector<int> messages = mailbox.drain();
  ok &= expect(messages == std::vector<int>({17, 23}),
               "drain must preserve publication order");
  ok &= expect(WaitForSingleObject(
                   static_cast<HANDLE>(mailbox.nativeWaitHandle().get()), 0) ==
                   WAIT_TIMEOUT,
               "drain must reset the mailbox wake event");
  ok &= expect(mailbox.drain().empty(),
               "draining an empty mailbox must return an empty batch");

  return ok ? 0 : 1;
}
