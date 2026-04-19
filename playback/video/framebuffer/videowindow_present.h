#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dxgi.h>

struct VideoWindowPresentArgs {
  UINT syncInterval = 0;
  UINT flags = 0;
};

inline VideoWindowPresentArgs liveVideoWindowPresentArgs(
    UINT configuredSyncInterval, bool nonBlocking) {
  if (nonBlocking) {
    return VideoWindowPresentArgs{0u, DXGI_PRESENT_DO_NOT_WAIT};
  }
  return VideoWindowPresentArgs{configuredSyncInterval, 0u};
}

inline bool videoWindowPresentSkipped(HRESULT hr) {
  return hr == DXGI_STATUS_OCCLUDED || hr == DXGI_ERROR_WAS_STILL_DRAWING;
}
