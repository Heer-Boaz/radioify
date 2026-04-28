#include "radioify_explorer_module.h"

namespace {

HMODULE g_module = nullptr;
LONG g_objectCount = 0;
LONG g_serverLockCount = 0;

}  // namespace

void radioifyExplorerSetModule(HMODULE module) noexcept {
  g_module = module;
}

std::wstring radioifyExplorerModulePath() {
  std::wstring buffer(MAX_PATH, L'\0');
  for (;;) {
    const DWORD length = GetModuleFileNameW(g_module, buffer.data(),
                                            static_cast<DWORD>(buffer.size()));
    if (length == 0) return {};
    if (length < buffer.size()) {
      buffer.resize(length);
      return buffer;
    }
    if (buffer.size() >= 32768) return {};
    buffer.resize(buffer.size() * 2);
  }
}

void radioifyExplorerObjectCreated() noexcept {
  InterlockedIncrement(&g_objectCount);
}

void radioifyExplorerObjectDestroyed() noexcept {
  InterlockedDecrement(&g_objectCount);
}

void radioifyExplorerLockServer(bool lock) noexcept {
  if (lock) {
    InterlockedIncrement(&g_serverLockCount);
  } else {
    InterlockedDecrement(&g_serverLockCount);
  }
}

bool radioifyExplorerCanUnload() noexcept {
  return g_objectCount == 0 && g_serverLockCount == 0;
}

HRESULT duplicateString(std::wstring_view text, LPWSTR* out) noexcept {
  if (!out) return E_POINTER;
  *out = nullptr;

  const size_t length = text.size() + 1;
  wchar_t* buffer =
      static_cast<wchar_t*>(CoTaskMemAlloc(length * sizeof(wchar_t)));
  if (!buffer) return E_OUTOFMEMORY;

  const HRESULT hr = StringCchCopyNW(buffer, length, text.data(), text.size());
  if (FAILED(hr)) {
    CoTaskMemFree(buffer);
    return hr;
  }

  *out = buffer;
  return S_OK;
}

HRESULT exceptionToHresult() noexcept {
  try {
    throw;
  } catch (const std::bad_alloc&) {
    return E_OUTOFMEMORY;
  } catch (...) {
    return E_FAIL;
  }
}
