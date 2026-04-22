#include "videowindow.h"

#include "videowindow_file_drop.h"

#include <memory>
#include <mutex>
#include <utility>

void VideoWindow::EnableFileDrop() {
    DisableFileDrop();
    if (!m_hWnd) {
        return;
    }

    auto target =
        std::make_unique<videowindow_file_drop::DropTargetRegistration>();
    if (!target->registerWindow(m_hWnd, [this](FileDropEvent&& drop) {
            InputEvent ev{};
            ev.type = InputEvent::Type::FileDrop;
            ev.fileDrop = std::move(drop);
            std::lock_guard<std::mutex> lock(m_inputMutex);
            m_inputQueue.push_back(std::move(ev));
        })) {
        return;
    }
    m_fileDropTarget = std::move(target);
}

void VideoWindow::DisableFileDrop() {
    m_fileDropTarget.reset();
}
