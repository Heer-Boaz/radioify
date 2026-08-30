#include "window.h"

#include "input_events.h"

#include <windowsx.h>

#include <atomic>
#include <utility>

bool VideoWindow::ShouldQueueWindowMouseEvent(int x, int y) const {
    if (m_captureAllMouseInput) {
        return true;
    }
    return OverlayInteractionAt(x, y);
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

    if (!pThis) {
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    if (uMsg == WM_NCHITTEST &&
        pThis->m_pictureInPicture.load(std::memory_order_relaxed)) {
        LRESULT hit = DefWindowProcW(hWnd, uMsg, wParam, lParam);
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
            POINT point{};
            bool resizeHandle = pThis->m_editBoundaryCaptureActive;
            if (!resizeHandle && ::GetCursorPos(&point) &&
                ::ScreenToClient(hWnd, &point)) {
                resizeHandle = pThis->OverlayEditBoundaryHandleAt(
                    point.x, point.y);
            }
            ::SetCursor(::LoadCursor(
                NULL, resizeHandle ? IDC_SIZEWE : IDC_ARROW));
        } else {
            ::SetCursor(nullptr);
        }
        return TRUE;
    }

    if (uMsg == WM_SIZE) {
        if (wParam != SIZE_MINIMIZED) {
            pThis->OnClientResizedByWindow(LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    }

    if (uMsg == WM_DISPLAYCHANGE) {
        RECT rect{};
        if (GetClientRect(hWnd, &rect)) {
            pThis->OnDisplayChangedByWindow(
                rect.right - rect.left, rect.bottom - rect.top);
        }
        return DefWindowProcW(hWnd, uMsg, wParam, lParam);
    }

    if (uMsg == WM_CLOSE) {
        pThis->RequestCloseFromWindow();
        return 0;
    }

    if (uMsg == WM_DESTROY) {
        pThis->m_input.endWindowThread();
        pThis->m_hWnd = nullptr;
        pThis->m_windowThreadId = 0;
        return 0;
    }

    if (window_input_events::isKeyDownMessage(uMsg, wParam)) {
        const WORD key = static_cast<WORD>(wParam);
        window_input_events::KeyDownTranslation translation =
            window_input_events::translateKeyDown(
                key, lParam, pThis->m_systemMediaCommandOwner);
        if (translation.event) {
            pThis->m_input.push(std::move(*translation.event));
            return 0;
        }
        if (translation.route ==
            window_input_events::KeyDownRoute::DelegateToDefaultWindowProcedure) {
            // DefWindowProc turns media virtual keys into WM_APPCOMMAND. That
            // message is Radioify's sole local media-command ingress.
            return DefWindowProcW(hWnd, uMsg, wParam, lParam);
        }
        return 0;
    }

    if (window_input_events::isSuppressedSystemCharacter(uMsg, wParam)) {
        return 0;
    }

    if (uMsg == WM_XBUTTONDOWN || uMsg == WM_NCXBUTTONDOWN) {
        if (auto event = window_input_events::inputEventFromXButton(wParam)) {
            pThis->m_input.push(std::move(*event));
            return TRUE;
        }
    }

    if (uMsg == WM_XBUTTONUP || uMsg == WM_NCXBUTTONUP ||
        uMsg == WM_XBUTTONDBLCLK || uMsg == WM_NCXBUTTONDBLCLK) {
        return TRUE;
    }

    if (uMsg == WM_APPCOMMAND) {
        window_input_events::AppCommandTranslation translation =
            window_input_events::translateAppCommand(
                lParam, pThis->m_systemMediaCommandOwner);
        if (translation.event) {
            pThis->m_input.push(std::move(*translation.event));
        }
        if (translation.handled) return TRUE;
    }

    auto queueWindowMouseEvent = [&](int x, int y, MouseEventKind kind,
                                     MouseButtons buttons, MouseButton button,
                                     int wheelDelta = 0) {
        const bool mouseCaptured = GetCapture() == hWnd;
        const bool pointerMove = kind == MouseEventKind::Move;
        if (!pointerMove &&
            !pThis->ShouldQueueWindowMouseEvent(x, y) && !mouseCaptured) {
            return;
        }
        if (pointerMove) {
            pThis->m_windowMouseInputActive = true;
        }
        pThis->m_input.push(window_input_events::mouseEvent(
            x, y, kind, buttons, button, wheelDelta));
    };

    if (uMsg == WM_NCMOUSEMOVE &&
        pThis->m_pictureInPicture.load(std::memory_order_relaxed)) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hWnd, &point);
        if (!pThis->m_trackingNonClientMouseLeave) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE | TME_NONCLIENT;
            tracking.hwndTrack = hWnd;
            pThis->m_trackingNonClientMouseLeave =
                TrackMouseEvent(&tracking) != FALSE;
        }
        queueWindowMouseEvent(point.x, point.y, MouseEventKind::Move,
                              MouseButtons::None, MouseButton::None);
        return 0;
    }

    if (uMsg == WM_NCMOUSELEAVE) {
        pThis->m_trackingNonClientMouseLeave = false;
        if (pThis->m_windowMouseInputActive && GetCapture() != hWnd) {
            pThis->m_windowMouseInputActive = false;
            pThis->m_input.push(window_input_events::pointerLeaveEvent());
        }
        return 0;
    }

    if (uMsg == WM_LBUTTONDBLCLK) {
        pThis->m_input.push(window_input_events::mouseEvent(
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
            MouseEventKind::DoubleClick, MouseButtons::Left,
            MouseButton::Left));
        return 0;
    }

    if (uMsg == WM_LBUTTONDOWN) {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (pThis->ShouldQueueWindowMouseEvent(x, y)) {
            const bool editBoundary =
                pThis->OverlayEditBoundaryHandleAt(x, y);
            SetCapture(hWnd);
            pThis->m_leftMouseCaptureActive = GetCapture() == hWnd;
            pThis->m_editBoundaryCaptureActive =
                pThis->m_leftMouseCaptureActive && editBoundary;
        }
        queueWindowMouseEvent(x, y, MouseEventKind::Press,
                              MouseButtons::Left, MouseButton::Left);
        return 0;
    }

    if (uMsg == WM_LBUTTONUP) {
        queueWindowMouseEvent(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
                              MouseEventKind::Release, MouseButtons::None,
                              MouseButton::Left);
        if (GetCapture() == hWnd) {
            pThis->m_leftMouseCaptureActive = false;
            pThis->m_editBoundaryCaptureActive = false;
            ReleaseCapture();
        }
        return 0;
    }

    if (uMsg == WM_CAPTURECHANGED) {
        if (pThis->m_leftMouseCaptureActive &&
            reinterpret_cast<HWND>(lParam) != hWnd) {
            pThis->m_leftMouseCaptureActive = false;
            pThis->m_editBoundaryCaptureActive = false;
            pThis->m_input.push(window_input_events::pointerLeaveEvent());
        }
        return 0;
    }

    if (uMsg == WM_CONTEXTMENU) {
        POINT point{};
        if (GET_X_LPARAM(lParam) == -1 && GET_Y_LPARAM(lParam) == -1) {
            RECT client{};
            if (GetClientRect(hWnd, &client)) {
                point.x = (client.right - client.left) / 2;
                point.y = (client.bottom - client.top) / 2;
            }
        } else {
            point = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ScreenToClient(hWnd, &point);
        }
        pThis->m_input.push(window_input_events::mouseEvent(
            point.x, point.y, MouseEventKind::Press, MouseButtons::Right,
            MouseButton::Right));
        return 0;
    }

    if (uMsg == WM_MOUSEMOVE) {
        if (!pThis->m_trackingMouseLeave) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = hWnd;
            pThis->m_trackingMouseLeave = TrackMouseEvent(&tracking) != FALSE;
        }
        queueWindowMouseEvent(
            GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam),
            MouseEventKind::Move,
            window_input_events::mouseButtonsFromWParam(wParam),
            MouseButton::None);
        return 0;
    }

    if (uMsg == WM_MOUSELEAVE) {
        pThis->m_trackingMouseLeave = false;
        if (pThis->m_windowMouseInputActive && GetCapture() != hWnd) {
            pThis->m_windowMouseInputActive = false;
            pThis->m_input.push(window_input_events::pointerLeaveEvent());
        }
        return 0;
    }

    if (uMsg == WM_MOUSEWHEEL) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(pThis->m_hWnd, &point);
        queueWindowMouseEvent(
            point.x, point.y, MouseEventKind::VerticalWheel,
            MouseButtons::None, MouseButton::None,
            GET_WHEEL_DELTA_WPARAM(wParam));
        return 0;
    }

    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}
