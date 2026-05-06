#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>

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

  return ok ? 0 : 1;
}
