#include "windows_notification_area_icon.h"

#include <deque>
#include <mutex>
#include <utility>

#include "waitable_signal.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>

#include <commctrl.h>
#include <shellapi.h>
#include <strsafe.h>

#include "windows_app_identity.h"
#include "windows_app_resources.h"

namespace {

constexpr wchar_t kNotificationAreaClassName[] =
    RADIOIFY_APP_NAME_W L".NotificationAreaIconWindow";
constexpr UINT kNotificationAreaCallbackMessage = WM_APP + 0x520;
constexpr UINT kNotificationAreaIconId = 1;
constexpr GUID kRadioifyNotificationAreaIconGuid = {
    0x493cbd6a,
    0xb852,
    0x4ece,
    {0x8f, 0xa4, 0x78, 0xb4, 0x83, 0x1f, 0x50, 0xb9}};

std::wstring utf8ToWide(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         value.data(),
                                         static_cast<int>(value.size()),
                                         nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring result(static_cast<size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                      static_cast<int>(value.size()), result.data(), length);
  return result;
}

std::wstring menuText(const std::string& label) {
  std::wstring result = utf8ToWide(label);
  return result.empty() ? RADIOIFY_APP_NAME_W : result;
}

std::wstring tooltipText(const std::string& tooltip) {
  std::wstring result = utf8ToWide(tooltip);
  if (result.empty()) {
    result = RADIOIFY_APP_NAME_W;
  }
  constexpr size_t kMaxTooltipCharacters = 127;
  if (result.size() > kMaxTooltipCharacters) {
    result.resize(kMaxTooltipCharacters);
  }
  return result;
}

struct UniqueIcon {
  UniqueIcon() = default;
  explicit UniqueIcon(HICON value) : icon(value) {}
  ~UniqueIcon() { reset(); }

  UniqueIcon(const UniqueIcon&) = delete;
  UniqueIcon& operator=(const UniqueIcon&) = delete;

  UniqueIcon(UniqueIcon&& other) noexcept
      : icon(std::exchange(other.icon, nullptr)) {}

  UniqueIcon& operator=(UniqueIcon&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.icon, nullptr));
    }
    return *this;
  }

  explicit operator bool() const { return icon != nullptr; }
  HICON get() const { return icon; }

  void reset(HICON next = nullptr) {
    if (icon) {
      DestroyIcon(icon);
    }
    icon = next;
  }

  HICON icon = nullptr;
};

struct UniqueMenu {
  UniqueMenu() = default;
  explicit UniqueMenu(HMENU value) : menu(value) {}
  ~UniqueMenu() { reset(); }

  UniqueMenu(const UniqueMenu&) = delete;
  UniqueMenu& operator=(const UniqueMenu&) = delete;

  UniqueMenu(UniqueMenu&& other) noexcept
      : menu(std::exchange(other.menu, nullptr)) {}

  UniqueMenu& operator=(UniqueMenu&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.menu, nullptr));
    }
    return *this;
  }

  explicit operator bool() const { return menu != nullptr; }
  HMENU get() const { return menu; }

  void reset(HMENU next = nullptr) {
    if (menu) {
      DestroyMenu(menu);
    }
    menu = next;
  }

  HMENU menu = nullptr;
};

}  // namespace

struct WindowsNotificationAreaIcon::Impl {
  explicit Impl(uint16_t resourceId) : iconResourceId(resourceId) {}

  uint16_t iconResourceId = 0;
  HINSTANCE instance = nullptr;
  ATOM classAtom = 0;
  HWND hwnd = nullptr;
  bool ownsClassRegistration = false;
  bool initialized = false;
  bool iconAdded = false;
  UINT taskbarCreatedMessage = 0;
  State state;
  UniqueIcon icon;
  WaitableSignal commandReady;
  mutable std::mutex stateMutex;
  std::mutex commandMutex;
  std::deque<uint32_t> pendingCommands;

  bool registerClass() {
    instance = GetModuleHandleW(nullptr);
    if (!instance) {
      return false;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kNotificationAreaClassName;

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

  bool createWindow() {
    if (hwnd) {
      return true;
    }
    if (!registerClass()) {
      return false;
    }
    taskbarCreatedMessage = RegisterWindowMessageW(L"TaskbarCreated");
    hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                           kNotificationAreaClassName, RADIOIFY_APP_NAME_W,
                           WS_POPUP | WS_DISABLED, 0, 0, 0, 0, nullptr,
                           nullptr, instance, this);
    if (!hwnd) {
      return false;
    }
    applyWindowsAppIdentityToWindow(hwnd);
    return true;
  }

  bool loadIcon() {
    HICON loadedIcon = nullptr;
    const HRESULT hr =
        LoadIconMetric(instance, MAKEINTRESOURCEW(iconResourceId), LIM_SMALL,
                       &loadedIcon);
    if (FAILED(hr) || !loadedIcon) {
      return false;
    }
    icon.reset(loadedIcon);
    return true;
  }

  NOTIFYICONDATAW notifyData(UINT flags) const {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = kNotificationAreaIconId;
    data.uFlags = flags | NIF_GUID;
    data.guidItem = kRadioifyNotificationAreaIconGuid;
    return data;
  }

  void fillPresentationData(NOTIFYICONDATAW* data) const {
    data->uFlags |= NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data->uCallbackMessage = kNotificationAreaCallbackMessage;
    data->hIcon = icon.get();
    const std::wstring tip = tooltipText(state.tooltip);
    StringCchCopyW(data->szTip, ARRAYSIZE(data->szTip), tip.c_str());
  }

  bool addIcon() {
    if (!hwnd || !icon) {
      return false;
    }
    NOTIFYICONDATAW data = notifyData(0);
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      fillPresentationData(&data);
    }
    if (!Shell_NotifyIconW(NIM_ADD, &data)) {
      iconAdded = false;
      return false;
    }

    NOTIFYICONDATAW versionData = notifyData(0);
    versionData.uVersion = NOTIFYICON_VERSION_4;
    if (!Shell_NotifyIconW(NIM_SETVERSION, &versionData)) {
      Shell_NotifyIconW(NIM_DELETE, &versionData);
      iconAdded = false;
      return false;
    }
    iconAdded = true;
    return true;
  }

  void modifyIcon() {
    if (!iconAdded) {
      (void)addIcon();
      return;
    }
    NOTIFYICONDATAW data = notifyData(0);
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      fillPresentationData(&data);
    }
    if (!Shell_NotifyIconW(NIM_MODIFY, &data)) {
      iconAdded = false;
    }
  }

  void deleteIcon() {
    if (!iconAdded) {
      return;
    }
    NOTIFYICONDATAW data = notifyData(0);
    Shell_NotifyIconW(NIM_DELETE, &data);
    iconAdded = false;
  }

  void restoreFocusToNotificationArea() {
    if (!iconAdded) {
      return;
    }
    NOTIFYICONDATAW data = notifyData(0);
    Shell_NotifyIconW(NIM_SETFOCUS, &data);
  }

  bool initialize(State initialState) {
    if (initialized) {
      update(std::move(initialState));
      return iconAdded;
    }
    state = std::move(initialState);
    if (!createWindow() || !loadIcon()) {
      return false;
    }
    initialized = true;
    return addIcon();
  }

  void update(State nextState) {
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      state = std::move(nextState);
    }
    if (initialized) {
      modifyIcon();
    }
  }

  void enqueueCommand(uint32_t commandId) {
    if (commandId == 0) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(commandMutex);
      pendingCommands.push_back(commandId);
    }
    commandReady.signal();
  }

  bool pollCommand(uint32_t* outCommandId) {
    if (!outCommandId) {
      return false;
    }
    std::lock_guard<std::mutex> lock(commandMutex);
    if (pendingCommands.empty()) {
      commandReady.clear();
      return false;
    }
    *outCommandId = pendingCommands.front();
    pendingCommands.pop_front();
    if (pendingCommands.empty()) {
      commandReady.clear();
    }
    return true;
  }

  NativeWaitHandle nativeWaitHandle() const {
    return commandReady.nativeWaitHandle();
  }

  UniqueMenu buildContextMenu() {
    State snapshot;
    {
      std::lock_guard<std::mutex> lock(stateMutex);
      snapshot = state;
    }
    UniqueMenu menu(CreatePopupMenu());
    if (!menu) {
      return {};
    }
    for (const MenuItem& item : snapshot.menuItems) {
      if (item.separatorBefore) {
        AppendMenuW(menu.get(), MF_SEPARATOR, 0, nullptr);
      }
      if (item.commandId == 0 || item.label.empty()) {
        continue;
      }
      UINT flags = MF_STRING;
      if (!item.enabled) {
        flags |= MF_GRAYED;
      }
      if (item.checked) {
        flags |= MF_CHECKED;
      }
      const std::wstring label = menuText(item.label);
      AppendMenuW(menu.get(), flags, item.commandId, label.c_str());
      if (item.defaultItem) {
        SetMenuDefaultItem(menu.get(), item.commandId, FALSE);
      }
    }
    return menu;
  }

  void showContextMenu(int x, int y) {
    UniqueMenu menu = buildContextMenu();
    if (!menu) {
      return;
    }

    SetForegroundWindow(hwnd);
    const UINT commandId = TrackPopupMenuEx(
        menu.get(), TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, x, y, hwnd,
        nullptr);
    if (commandId != 0) {
      enqueueCommand(commandId);
    }
    restoreFocusToNotificationArea();
  }

  LRESULT handleNotificationMessage(WPARAM wParam, LPARAM lParam) {
    const UINT event = LOWORD(lParam);
    const int x = GET_X_LPARAM(wParam);
    const int y = GET_Y_LPARAM(wParam);
    if (event == NIN_SELECT || event == NIN_KEYSELECT) {
      State snapshot;
      {
        std::lock_guard<std::mutex> lock(stateMutex);
        snapshot = state;
      }
      enqueueCommand(snapshot.defaultCommandId);
      return 0;
    }
    if (event == WM_CONTEXTMENU) {
      showContextMenu(x, y);
      return 0;
    }
    return 0;
  }

  static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam,
                                     LPARAM lParam) {
    Impl* self = nullptr;
    if (message == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
      self = static_cast<Impl*>(create->lpCreateParams);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
      self = reinterpret_cast<Impl*>(
          GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!self) {
      return DefWindowProcW(hwnd, message, wParam, lParam);
    }
    if (message == kNotificationAreaCallbackMessage) {
      return self->handleNotificationMessage(wParam, lParam);
    }
    if (self->taskbarCreatedMessage != 0 &&
        message == self->taskbarCreatedMessage) {
      self->iconAdded = false;
      self->addIcon();
      return 0;
    }
    if (message == WM_DESTROY) {
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      self->hwnd = nullptr;
      return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
  }

  ~Impl() {
    deleteIcon();
    if (hwnd) {
      DestroyWindow(hwnd);
      hwnd = nullptr;
    }
    if (ownsClassRegistration && instance) {
      UnregisterClassW(kNotificationAreaClassName, instance);
    }
  }
};

#else

struct WindowsNotificationAreaIcon::Impl {
  explicit Impl(uint16_t) {}

  WaitableSignal commandReady;

  bool initialize(State) { return false; }
  bool available() const { return false; }
  void update(State) {}
  bool pollCommand(uint32_t*) { return false; }
  NativeWaitHandle nativeWaitHandle() const {
    return commandReady.nativeWaitHandle();
  }
};

#endif

WindowsNotificationAreaIcon::WindowsNotificationAreaIcon(uint16_t iconResourceId)
    : impl_(std::make_unique<Impl>(iconResourceId)) {}

WindowsNotificationAreaIcon::~WindowsNotificationAreaIcon() = default;

WindowsNotificationAreaIcon::WindowsNotificationAreaIcon(
    WindowsNotificationAreaIcon&&) noexcept = default;

WindowsNotificationAreaIcon& WindowsNotificationAreaIcon::operator=(
    WindowsNotificationAreaIcon&&) noexcept = default;

bool WindowsNotificationAreaIcon::initialize(State state) {
  return impl_->initialize(std::move(state));
}

bool WindowsNotificationAreaIcon::available() const {
#ifdef _WIN32
  return impl_->iconAdded;
#else
  return impl_->available();
#endif
}

void WindowsNotificationAreaIcon::update(State state) {
  impl_->update(std::move(state));
}

bool WindowsNotificationAreaIcon::pollCommand(uint32_t* outCommandId) {
  return impl_->pollCommand(outCommandId);
}

NativeWaitHandle WindowsNotificationAreaIcon::nativeWaitHandle() const {
  return impl_->nativeWaitHandle();
}
