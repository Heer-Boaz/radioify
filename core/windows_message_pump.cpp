#include "windows_message_pump.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <system_error>

bool pumpPendingThreadWindowMessages() {
  bool handledMessages = false;
  MSG msg{};
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    handledMessages = true;
    if (msg.message == WM_QUIT) {
      PostQuitMessage(static_cast<int>(msg.wParam));
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  return handledMessages;
}

DWORD waitForHandlesAndPumpThreadWindowMessages(
    DWORD handleCount, const NativeWaitHandle* handles,
    wake_schedule::Deadline deadline) {
  if (handleCount > 0 && !handles) {
    throw std::invalid_argument(
        "A non-empty Windows wait set requires a handle array.");
  }

  HANDLE waitHandles[kMaximumThreadMessageWaitHandles];
  DWORD waitHandleCount = 0;
  for (DWORD i = 0; i < handleCount; ++i) {
    if (!handles[i]) continue;
    const HANDLE candidate = static_cast<HANDLE>(handles[i].get());
    bool duplicate = false;
    for (DWORD existing = 0; existing < waitHandleCount; ++existing) {
      if (waitHandles[existing] == candidate) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) continue;
    if (waitHandleCount == kMaximumThreadMessageWaitHandles) {
      throw std::length_error(
          "The Windows thread-message wait set exceeds its native capacity.");
    }
    waitHandles[waitHandleCount++] = candidate;
  }

  for (;;) {
    DWORD waitMs = INFINITE;
    if (deadline) {
      const auto remaining = *deadline - wake_schedule::Clock::now();
      if (remaining <= wake_schedule::Clock::duration::zero()) {
        waitMs = 0;
      } else {
        const auto remainingMs =
            std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
        waitMs = static_cast<DWORD>((std::min)(
            remainingMs, static_cast<decltype(remainingMs)>(INFINITE - 1)));
      }
    }

    const DWORD result =
        MsgWaitForMultipleObjectsEx(
            waitHandleCount, waitHandleCount > 0 ? waitHandles : nullptr,
            waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_OBJECT_0 + waitHandleCount) {
      pumpPendingThreadWindowMessages();
      continue;
    }
    if (result == WAIT_FAILED) {
      throw std::system_error(
          static_cast<int>(GetLastError()), std::system_category(),
          "MsgWaitForMultipleObjectsEx failed");
    }
    return result;
  }
}
