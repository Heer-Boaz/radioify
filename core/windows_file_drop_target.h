#pragma once

#include <functional>
#include <optional>

#include "file_drop_event.h"
#include "windows_file_drop_apartment.h"

namespace windows_file_drop {

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
  std::optional<OleApartment> oleApartment_;
};

}  // namespace windows_file_drop
