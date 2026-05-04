#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native_wait_handle.h"

bool pumpPendingThreadWindowMessages();

DWORD waitForHandlesAndPumpThreadWindowMessages(DWORD handleCount,
                                                const NativeWaitHandle* handles,
                                                DWORD timeoutMs);
