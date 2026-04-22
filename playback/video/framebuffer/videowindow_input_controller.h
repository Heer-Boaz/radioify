#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <memory>

#include "consoleinput.h"
#include "videowindow_input_queue.h"

namespace windows_file_drop {
class DropTargetRegistration;
}

class VideoWindowInputController {
 public:
  VideoWindowInputController();
  ~VideoWindowInputController();

  VideoWindowInputController(const VideoWindowInputController&) = delete;
  VideoWindowInputController& operator=(const VideoWindowInputController&) =
      delete;

  void clear();
  void push(InputEvent ev);
  bool poll(InputEvent& ev);

  bool enableFileDrop(HWND hwnd);
  void disableFileDrop();

 private:
  VideoWindowInputQueue events_;
  std::unique_ptr<windows_file_drop::DropTargetRegistration> fileDropTarget_;
};
