#include "host_window.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <memory>
#include <utility>

#include "core/windows_app_identity.h"

namespace {

constexpr wchar_t kHostWindowClassName[] =
    L"Radioify.SystemMediaTransportHostWindow";

LRESULT CALLBACK hostWindowProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace

struct PlaybackSystemTransportHostWindow::Impl {
  HINSTANCE instance = nullptr;
  ATOM classAtom = 0;
  HWND hwnd = nullptr;
  bool ownsClassRegistration = false;

  bool registerClass() {
    instance = GetModuleHandleW(nullptr);
    if (!instance) {
      return false;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = hostWindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kHostWindowClassName;

    classAtom = RegisterClassW(&wc);
    if (classAtom != 0) {
      ownsClassRegistration = true;
      return true;
    }

    const DWORD error = GetLastError();
    if (error != ERROR_CLASS_ALREADY_EXISTS) {
      return false;
    }

    classAtom = 1;
    return true;
  }

  bool initialize() {
    if (hwnd) {
      return true;
    }
    if (!registerClass()) {
      return false;
    }

    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           kHostWindowClassName, L"Radioify",
                           WS_POPUP | WS_DISABLED, 0, 0, 0, 0, nullptr,
                           nullptr, instance, nullptr);
    if (!hwnd) {
      return false;
    }

    applyWindowsAppIdentityToWindow(hwnd);
    return true;
  }

  ~Impl() {
    if (hwnd) {
      DestroyWindow(hwnd);
      hwnd = nullptr;
    }
    if (ownsClassRegistration && instance) {
      UnregisterClassW(kHostWindowClassName, instance);
    }
  }
};

PlaybackSystemTransportHostWindow::PlaybackSystemTransportHostWindow()
    : impl_(new Impl()) {}

PlaybackSystemTransportHostWindow::~PlaybackSystemTransportHostWindow() {
  delete impl_;
}

PlaybackSystemTransportHostWindow::PlaybackSystemTransportHostWindow(
    PlaybackSystemTransportHostWindow&& other) noexcept
    : impl_(other.impl_) {
  other.impl_ = nullptr;
}

PlaybackSystemTransportHostWindow& PlaybackSystemTransportHostWindow::operator=(
    PlaybackSystemTransportHostWindow&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  delete impl_;
  impl_ = other.impl_;
  other.impl_ = nullptr;
  return *this;
}

bool PlaybackSystemTransportHostWindow::initialize() {
  return impl_ && impl_->initialize();
}

void* PlaybackSystemTransportHostWindow::nativeHandle() const {
  return impl_ ? impl_->hwnd : nullptr;
}

#else

PlaybackSystemTransportHostWindow::PlaybackSystemTransportHostWindow() = default;
PlaybackSystemTransportHostWindow::~PlaybackSystemTransportHostWindow() = default;

PlaybackSystemTransportHostWindow::PlaybackSystemTransportHostWindow(
    PlaybackSystemTransportHostWindow&& other) noexcept
    : impl_(other.impl_) {
  other.impl_ = nullptr;
}

PlaybackSystemTransportHostWindow& PlaybackSystemTransportHostWindow::operator=(
    PlaybackSystemTransportHostWindow&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  impl_ = other.impl_;
  other.impl_ = nullptr;
  return *this;
}

bool PlaybackSystemTransportHostWindow::initialize() {
  return false;
}

void* PlaybackSystemTransportHostWindow::nativeHandle() const {
  return nullptr;
}

#endif
