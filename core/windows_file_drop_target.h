#pragma once

#include <functional>
#include <memory>

#include "file_drop_event.h"
#include "windows_file_drop_acceptance.h"

struct HWND__;
using HWND = HWND__*;

namespace windows_file_drop {

using DropEventSink = std::function<void(FileDropEvent&&)>;

class DropTargetRegistration {
 public:
  DropTargetRegistration() = default;
  ~DropTargetRegistration();

  DropTargetRegistration(const DropTargetRegistration&) = delete;
  DropTargetRegistration& operator=(const DropTargetRegistration&) = delete;

  bool registerWindow(HWND hwnd, DropEventSink sink,
                      std::shared_ptr<AcceptanceState> acceptance);
  void revoke();

 private:
  HWND hwnd_ = nullptr;
  class DropTarget* target_ = nullptr;
};

}  // namespace windows_file_drop
