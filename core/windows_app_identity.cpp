#include "windows_app_identity.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shobjidl_core.h>

#include <filesystem>
#include <string>
#include <vector>
#include <wrl/client.h>

#include "runtime_helpers.h"

namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kRadioifyAppUserModelId[] = L"Radioify.App";
constexpr wchar_t kRadioifyDisplayName[] = L"Radioify";

std::wstring executablePathOrEmpty() {
  std::wstring buffer;
  DWORD size = MAX_PATH;
  for (;;) {
    buffer.resize(size);
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), size);
    if (length == 0) {
      return {};
    }
    if (length < size) {
      buffer.resize(length);
      return buffer;
    }
    size *= 2;
    if (size > 32768) {
      return {};
    }
  }
}

std::wstring quoteCommandLineArgument(const std::wstring& value) {
  std::wstring quoted;
  quoted.reserve(value.size() + 2);
  quoted.push_back(L'"');
  for (wchar_t ch : value) {
    if (ch == L'"') {
      quoted.push_back(L'\\');
    }
    quoted.push_back(ch);
  }
  quoted.push_back(L'"');
  return quoted;
}

std::wstring relaunchCommandOrEmpty() {
  const std::wstring exePath = executablePathOrEmpty();
  if (exePath.empty()) {
    return {};
  }
  return quoteCommandLineArgument(exePath);
}

std::wstring relaunchIconResourceOrEmpty() {
  const std::vector<std::filesystem::path> roots = radioifyResourceSearchRoots();
  for (const auto& root : roots) {
    if (root.empty()) {
      continue;
    }

    const std::filesystem::path iconPath = root / "radioify.ico";
    std::error_code ec;
    if (!std::filesystem::is_regular_file(iconPath, ec) || ec) {
      continue;
    }

    return iconPath.native() + L",0";
  }
  return {};
}

bool setWindowPropertyString(IPropertyStore* propertyStore,
                             REFPROPERTYKEY key,
                             const std::wstring& value) {
  if (!propertyStore || value.empty()) {
    return false;
  }

  PROPVARIANT propValue;
  PropVariantInit(&propValue);
  const HRESULT initHr = InitPropVariantFromString(value.c_str(), &propValue);
  if (FAILED(initHr)) {
    PropVariantClear(&propValue);
    return false;
  }

  const HRESULT setHr = propertyStore->SetValue(key, propValue);
  PropVariantClear(&propValue);
  return SUCCEEDED(setHr);
}

void applyWindowAppIdentity(HWND hwnd) {
  if (!hwnd) {
    return;
  }

  ComPtr<IPropertyStore> propertyStore;
  const HRESULT hr =
      SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&propertyStore));
  if (FAILED(hr) || !propertyStore) {
    return;
  }

  const std::wstring relaunchCommand = relaunchCommandOrEmpty();
  const std::wstring relaunchIconResource = relaunchIconResourceOrEmpty();

  bool changed = false;
  changed |= setWindowPropertyString(propertyStore.Get(), PKEY_AppUserModel_ID,
                                     kRadioifyAppUserModelId);
  changed |= setWindowPropertyString(
      propertyStore.Get(), PKEY_AppUserModel_RelaunchDisplayNameResource,
      kRadioifyDisplayName);
  changed |= setWindowPropertyString(
      propertyStore.Get(), PKEY_AppUserModel_RelaunchCommand, relaunchCommand);
  if (!relaunchIconResource.empty()) {
    changed |= setWindowPropertyString(
        propertyStore.Get(), PKEY_AppUserModel_RelaunchIconResource,
        relaunchIconResource);
  }

  if (changed) {
    propertyStore->Commit();
  }
}

}  // namespace

void initializeWindowsAppIdentity() {
  const HRESULT hr =
      SetCurrentProcessExplicitAppUserModelID(kRadioifyAppUserModelId);
  if (FAILED(hr)) {
    return;
  }
}

void applyWindowsAppIdentityToWindow(void* nativeWindowHandle) {
  applyWindowAppIdentity(static_cast<HWND>(nativeWindowHandle));
}

#else

void initializeWindowsAppIdentity() {}
void applyWindowsAppIdentityToWindow(void*) {}

#endif
