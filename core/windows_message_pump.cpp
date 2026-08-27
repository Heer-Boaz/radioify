#include "windows_message_pump.h"

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
    DWORD handleCount, const NativeWaitHandle* handles, DWORD timeoutMs) {
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

  const bool infiniteTimeout = timeoutMs == INFINITE;
  const ULONGLONG deadlineTick =
      infiniteTimeout ? 0 : (GetTickCount64() + static_cast<ULONGLONG>(timeoutMs));

  for (;;) {
    DWORD waitMs = INFINITE;
    if (!infiniteTimeout) {
      const ULONGLONG nowTick = GetTickCount64();
      waitMs = nowTick >= deadlineTick
                   ? 0
                   : static_cast<DWORD>(deadlineTick - nowTick);
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
