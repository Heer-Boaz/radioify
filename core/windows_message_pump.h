#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

bool pumpPendingThreadWindowMessages();

DWORD waitForHandlesAndPumpThreadWindowMessages(DWORD handleCount,
                                                const HANDLE* handles,
                                                DWORD timeoutMs);
