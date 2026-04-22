#include "videowindow_input_controller.h"

#include "windows_file_drop_target.h"

#include <utility>

VideoWindowInputController::VideoWindowInputController() = default;

VideoWindowInputController::~VideoWindowInputController() = default;

void VideoWindowInputController::clear() {
  events_.clear();
}

void VideoWindowInputController::push(InputEvent ev) {
  events_.push(std::move(ev));
}

bool VideoWindowInputController::poll(InputEvent& ev) {
  return events_.poll(ev);
}

bool VideoWindowInputController::enableFileDrop(HWND hwnd) {
  disableFileDrop();
  if (!hwnd) {
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

void VideoWindowInputController::disableFileDrop() {
  fileDropTarget_.reset();
}
