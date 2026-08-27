#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "native_wait_handle.h"
#include "wake_deadline.h"

inline constexpr DWORD kMaximumThreadMessageWaitHandles =
    MAXIMUM_WAIT_OBJECTS - 1;

bool pumpPendingThreadWindowMessages();

// Waits for unique, valid handles while continuing to dispatch messages for
// windows owned by the calling thread. Throws when the input contract cannot
// be represented by MsgWaitForMultipleObjectsEx; handles are never dropped.
DWORD waitForHandlesAndPumpThreadWindowMessages(DWORD handleCount,
                                                const NativeWaitHandle* handles,
                                                wake_schedule::Deadline deadline);
