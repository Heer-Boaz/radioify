#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>
#include <optional>

#include "consoleinput.h"
#include "windows_file_drop_apartment.h"
#include "input_queue.h"

namespace windows_file_drop {
class DropTargetRegistration;
}

class WindowInputController {
 public:
  WindowInputController();
  ~WindowInputController();

  WindowInputController(const WindowInputController&) = delete;
  WindowInputController& operator=(const WindowInputController&) =
      delete;

  void clear();
  void push(InputEvent ev);
  bool poll(InputEvent& ev);

  bool beginWindowThread();
  void endWindowThread();
  bool enableFileDrop(HWND hwnd);
  void disableFileDrop();

 private:
  WindowInputQueue events_;
  std::optional<windows_file_drop::OleApartment> fileDropApartment_;
  std::unique_ptr<windows_file_drop::DropTargetRegistration> fileDropTarget_;
};
