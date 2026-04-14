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

DWORD waitForHandlesAndPumpThreadWindowMessages(DWORD handleCount,
                                                const HANDLE* handles,
                                                DWORD timeoutMs) {
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
        MsgWaitForMultipleObjectsEx(handleCount, handleCount > 0 ? handles : nullptr,
                                    waitMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (result == WAIT_OBJECT_0 + handleCount) {
      pumpPendingThreadWindowMessages();
      continue;
    }
    return result;
  }
}
