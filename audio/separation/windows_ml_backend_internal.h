#pragma once

#include <windows.h>
#include <WinMLAsync.h>

#include <functional>
#include <optional>

#include "audio/separation/windows_ml_backend.h"

namespace audio_separation::detail {

struct AsyncProviderSetupApi {
  std::function<HRESULT(WinMLAsyncBlock*)> start;
  std::function<HRESULT(WinMLAsyncBlock*)> cancel;
  std::function<HRESULT(WinMLAsyncBlock*, BOOL)> getStatus;
  std::function<void(WinMLAsyncBlock*)> close;
};

struct AsyncProviderSetupExecution {
  HRESULT startResult = E_FAIL;
  HRESULT statusResult = E_FAIL;
  bool cancellationIssued = false;
  std::optional<HRESULT> cancellationFailure;

  bool started() const { return SUCCEEDED(startResult); }
};

// Bridges the callback-based Windows ML ABI to Radioify's worker-thread
// progress/cancellation contract. C callbacks only signal private state; all
// application callbacks run on the worker that called this function.
AsyncProviderSetupExecution runAsyncProviderSetup(
    const AsyncProviderSetupApi& api,
    const WindowsMlProviderSetupProgress& reportProgress,
    const WindowsMlProviderSetupCancellation& cancellationRequested);

}  // namespace audio_separation::detail
