#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "native_wait_handle.h"
#include "windows_app_resources.h"

class WindowsNotificationAreaIcon {
 public:
  struct MenuItem {
    uint32_t commandId = 0;
    std::string label;
    bool enabled = true;
    bool checked = false;
    bool separatorBefore = false;
    bool defaultItem = false;
  };

  struct State {
    std::string tooltip = RADIOIFY_APP_NAME;
    std::vector<MenuItem> menuItems;
    uint32_t defaultCommandId = 0;
  };

  explicit WindowsNotificationAreaIcon(uint16_t iconResourceId);
  ~WindowsNotificationAreaIcon();

  WindowsNotificationAreaIcon(WindowsNotificationAreaIcon&&) noexcept;
  WindowsNotificationAreaIcon& operator=(WindowsNotificationAreaIcon&&) noexcept;

  WindowsNotificationAreaIcon(const WindowsNotificationAreaIcon&) = delete;
  WindowsNotificationAreaIcon& operator=(const WindowsNotificationAreaIcon&) =
      delete;

  bool initialize(State state);
  bool available() const;
  void update(State state);
  bool pollCommand(uint32_t* outCommandId);
  NativeWaitHandle nativeWaitHandle() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
