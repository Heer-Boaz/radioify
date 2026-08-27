#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <stdexcept>
#include <vector>

#include "core/windows_handle.h"
#include "core/windows_message_pump.h"

namespace {

bool expect(bool condition, const char* message) {
  if (condition) return true;
  std::fprintf(stderr, "windows_message_pump_tests: %s\n", message);
  return false;
}

UniqueWindowsHandle makeEvent(bool signaled = false) {
  return UniqueWindowsHandle(
      CreateEventW(nullptr, TRUE, signaled ? TRUE : FALSE, nullptr));
}

}  // namespace

int main() {
  bool ok = true;

  UniqueWindowsHandle signaled = makeEvent(true);
  NativeWaitHandle signaledHandle(signaled.get());
  ok &= expect(
      waitForHandlesAndPumpThreadWindowMessages(1, &signaledHandle, 0) ==
          WAIT_OBJECT_0,
      "a zero-duration wait must still observe an already-signaled handle");

  bool rejectedMissingArray = false;
  try {
    (void)waitForHandlesAndPumpThreadWindowMessages(1, nullptr, 0);
  } catch (const std::invalid_argument&) {
    rejectedMissingArray = true;
  }
  ok &= expect(rejectedMissingArray,
               "a non-empty wait set must require an input array");

  std::vector<UniqueWindowsHandle> ownedHandles;
  std::vector<NativeWaitHandle> uniqueHandles;
  ownedHandles.reserve(kMaximumThreadMessageWaitHandles + 1);
  uniqueHandles.reserve(kMaximumThreadMessageWaitHandles + 1);
  for (DWORD index = 0; index < kMaximumThreadMessageWaitHandles + 1;
       ++index) {
    ownedHandles.push_back(makeEvent());
    ok &= expect(static_cast<bool>(ownedHandles.back()),
                 "test event creation must succeed");
    uniqueHandles.emplace_back(ownedHandles.back().get());
  }

  ok &= expect(waitForHandlesAndPumpThreadWindowMessages(
                   kMaximumThreadMessageWaitHandles, uniqueHandles.data(), 0) ==
                   WAIT_TIMEOUT,
               "the documented maximum unique wait set must be accepted");

  bool rejectedOverflow = false;
  try {
    (void)waitForHandlesAndPumpThreadWindowMessages(
        static_cast<DWORD>(uniqueHandles.size()), uniqueHandles.data(), 0);
  } catch (const std::length_error&) {
    rejectedOverflow = true;
  }
  ok &= expect(rejectedOverflow,
               "a wait-set overflow must fail instead of dropping handles");

  std::vector<NativeWaitHandle> duplicates(
      kMaximumThreadMessageWaitHandles + 1, signaledHandle);
  ok &= expect(waitForHandlesAndPumpThreadWindowMessages(
                   static_cast<DWORD>(duplicates.size()), duplicates.data(),
                   0) == WAIT_OBJECT_0,
               "duplicate handles must be coalesced before enforcing capacity");

  return ok ? 0 : 1;
}
