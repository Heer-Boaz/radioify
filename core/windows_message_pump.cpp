#include "windows_message_pump.h"

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
  HANDLE waitHandles[MAXIMUM_WAIT_OBJECTS];
  DWORD waitHandleCount = 0;
  for (DWORD i = 0; i < handleCount && waitHandleCount < MAXIMUM_WAIT_OBJECTS;
       ++i) {
    if (handles[i]) {
      waitHandles[waitHandleCount++] = static_cast<HANDLE>(handles[i].get());
    }
  }

  const bool infiniteTimeout = timeoutMs == INFINITE;
  const ULONGLONG deadlineTick =
      infiniteTimeout ? 0 : (GetTickCount64() + static_cast<ULONGLONG>(timeoutMs));

  for (;;) {
    DWORD waitMs = INFINITE;
    if (!infiniteTimeout) {
      const ULONGLONG nowTick = GetTickCount64();
      if (nowTick >= deadlineTick) {
        return WAIT_TIMEOUT;
      }
      waitMs = static_cast<DWORD>(deadlineTick - nowTick);
    }

    const DWORD result =
        MsgWaitForMultipleObjectsEx(
            waitHandleCount, waitHandleCount > 0 ? waitHandles : nullptr,
            waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_OBJECT_0 + waitHandleCount) {
      pumpPendingThreadWindowMessages();
      continue;
    }
    return result;
  }
}
