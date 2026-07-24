#include "input_controller.h"

#include "windows_file_drop_target.h"

#include <utility>

WindowInputController::WindowInputController() = default;

WindowInputController::~WindowInputController() {
  endWindowThread();
}

void WindowInputController::clear() {
  events_.clear();
}

void WindowInputController::push(InputEvent ev) {
  events_.push(std::move(ev));
}

bool WindowInputController::poll(InputEvent& ev) {
  return events_.poll(ev);
}

NativeWaitHandle WindowInputController::nativeWaitHandle() const {
  return events_.nativeWaitHandle();
}

bool WindowInputController::beginWindowThread() {
  if (fileDropApartment_ || fileDropTarget_) {
    return false;
  }
  fileDropApartment_.emplace();
  if (!fileDropApartment_->initialized()) {
    fileDropApartment_.reset();
    return false;
  }
  return true;
}

void WindowInputController::endWindowThread() {
  disableFileDrop();
  events_.clear();
  fileDropApartment_.reset();
}

bool WindowInputController::enableFileDrop(HWND hwnd) {
  if (!hwnd || !fileDropApartment_ || fileDropTarget_) {
    return false;
  }

  auto target = std::make_unique<windows_file_drop::DropTargetRegistration>();
  if (!target->registerWindow(hwnd, [this](FileDropEvent&& drop) {
        InputEvent ev{};
        ev.type = InputEvent::Type::FileDrop;
        ev.fileDrop = std::move(drop);
        push(std::move(ev));
      })) {
    return false;
  }

  fileDropTarget_ = std::move(target);
  return true;
}

void WindowInputController::disableFileDrop() {
  fileDropTarget_.reset();
}
