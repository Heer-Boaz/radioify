#pragma once

#include <functional>

#include "file_drop_event.h"

struct HWND__;
using HWND = HWND__*;

namespace videowindow_file_drop {

using DropEventSink = std::function<void(FileDropEvent&&)>;

class DropTargetRegistration {
 public:
  DropTargetRegistration() = default;
  ~DropTargetRegistration();

  DropTargetRegistration(const DropTargetRegistration&) = delete;
  DropTargetRegistration& operator=(const DropTargetRegistration&) = delete;

  bool registerWindow(HWND hwnd, DropEventSink sink);
  void revoke();

 private:
  HWND hwnd_ = nullptr;
  class DropTarget* target_ = nullptr;
  bool oleInitialized_ = false;
};

}  // namespace videowindow_file_drop
