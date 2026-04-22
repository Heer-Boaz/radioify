#include "videowindow.h"

bool VideoWindow::EnableFileDrop() {
  return m_input.enableFileDrop(m_hWnd);
}

void VideoWindow::DisableFileDrop() {
  m_input.disableFileDrop();
}
