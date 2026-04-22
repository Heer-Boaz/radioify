#include "videowindow.h"

#include "videowindow_input_events.h"

#include <windowsx.h>

#include <atomic>
#include <cmath>
#include <utility>

bool VideoWindow::ShouldQueueWindowMouseEvent(int y) const {
    if (m_captureAllMouseInput) {
        return true;
    }
    if (m_height <= 0) {
        return false;
    }
    if (m_pictureInPicture.load(std::memory_order_relaxed)) {
        return y >= PictureInPictureInteractiveTop();
    }
    return y > static_cast<int>(std::round(m_height * 0.84));
}

LRESULT CALLBACK VideoWindow::WindowProc(HWND hWnd, UINT uMsg, WPARAM wParam,
                                         LPARAM lParam) {
    VideoWindow* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        auto* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        pThis = static_cast<VideoWindow*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hWnd, GWLP_USERDATA,
                         reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis =
            reinterpret_cast<VideoWindow*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    }

    if (pThis && uMsg == WM_SIZE && pThis->m_ignoreWindowSizeEvents) {
        return 0;
    }

    if (!pThis) {
        return DefWindowProc(hWnd, uMsg, wParam, lParam);
    }

    if (uMsg == kTogglePictureInPictureMessage) {
        pThis->SetPictureInPicture(!pThis->IsPictureInPicture());
        return 0;
    }
    if (uMsg == kSetPictureInPictureMessage) {
        pThis->SetPictureInPicture(wParam != 0);
        return 0;
    }
    if (uMsg == kExitPictureInPictureToFullscreenMessage) {
        pThis->ExitPictureInPictureToFullscreen();
        return 0;
    }
    if (uMsg == kSetFullscreenMessage) {
        pThis->SetFullscreen(wParam != 0);
        return 0;
    }

    if (uMsg == WM_NCHITTEST &&
        pThis->m_pictureInPicture.load(std::memory_order_relaxed)) {
        LRESULT hit = DefWindowProc(hWnd, uMsg, wParam, lParam);
        if (hit != HTCLIENT) {
            return hit;
        }
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hWnd, &point);
        return pThis->HitTestPictureInPicture(point.x, point.y);
    }

    if (uMsg == WM_SIZING &&
        pThis->m_pictureInPicture.load(std::memory_order_relaxed)) {
        pThis->AdjustPictureInPictureSizingRect(
            wParam, reinterpret_cast<RECT*>(lParam));
        return TRUE;
    }

    if (uMsg == WM_GETMINMAXINFO &&
        pThis->m_pictureInPicture.load(std::memory_order_relaxed)) {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        const SIZE minSize = pThis->PictureInPictureMinimumSize();
        info->ptMinTrackSize.x = minSize.cx;
        info->ptMinTrackSize.y = minSize.cy;
        return 0;
    }

    if (uMsg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) {
        if (pThis->m_cursorVisible.load(std::memory_order_relaxed)) {
            ::SetCursor(::LoadCursor(NULL, IDC_ARROW));
        } else {
            ::SetCursor(nullptr);
        }
        return TRUE;
    }

    if (uMsg == WM_SIZE) {
        pThis->Resize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    }

    if (uMsg == WM_CLOSE) {
        pThis->m_closeRequested.store(true, std::memory_order_relaxed);
        if (pThis->m_closeRequestedEvent) {
            SetEvent(pThis->m_closeRequestedEvent);
        }
        return 0;
    }

    if (uMsg == WM_DESTROY) {
        pThis->m_hWnd = nullptr;
        pThis->m_windowThreadId = 0;
        return 0;
    }

    if (videowindow_input_events::isKeyDownMessage(uMsg, wParam)) {
        pThis->m_input.push(
            videowindow_input_events::keyFromVirtualKey(
                static_cast<WORD>(wParam)));
        return 0;
    }

    if (videowindow_input_events::isSuppressedSystemCharacter(uMsg, wParam)) {
        return 0;
    }

    if (videowindow_input_events::isXButtonMessage(uMsg)) {
        if (auto event =
                videowindow_input_events::keyFromXButtonMessage(wParam)) {
            pThis->m_input.push(std::move(*event));
        }
        return TRUE;
    }

    if (uMsg == WM_APPCOMMAND) {
        if (auto event = videowindow_input_events::keyFromAppCommand(lParam)) {
            pThis->m_input.push(std::move(*event));
            return TRUE;
        }
    }

    auto queueWindowMouseEvent = [&](int x, int y, DWORD buttonState,
                                     DWORD eventFlags) {
        if (!pThis->ShouldQueueWindowMouseEvent(y)) {
            return;
        }
        pThis->m_input.push(videowindow_input_events::mouseEvent(
            x, y, buttonState, eventFlags));
    };

    if (uMsg == WM_LBUTTONDOWN) {
        queueWindowMouseEvent(LOWORD(lParam), HIWORD(lParam),
                              FROM_LEFT_1ST_BUTTON_PRESSED, 0);
        return 0;
    }

    if (uMsg == WM_RBUTTONDOWN) {
        queueWindowMouseEvent(LOWORD(lParam), HIWORD(lParam),
                              RIGHTMOST_BUTTON_PRESSED, 0);
        return 0;
    }

    if (uMsg == WM_MOUSEMOVE) {
        queueWindowMouseEvent(
            LOWORD(lParam), HIWORD(lParam),
            videowindow_input_events::mouseButtonsFromWParam(wParam),
            MOUSE_MOVED);
        return 0;
    }

    if (uMsg == WM_MOUSEWHEEL) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(pThis->m_hWnd, &point);
        queueWindowMouseEvent(
            point.x, point.y,
            videowindow_input_events::wheelButtonState(
                GET_WHEEL_DELTA_WPARAM(wParam)),
            MOUSE_WHEELED);
        return 0;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}
