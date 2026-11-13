#include "overlay.hpp"
#include "rendering/probe_card.hpp"

#include <cstdlib>
#include <utility>

namespace cliprope::win {
namespace {
constexpr auto class_name = L"ClipRope.Probe.Overlay";
constexpr int host_width = 320, host_height = 154;
int pixels(int dip, UINT dpi) { return MulDiv(dip, static_cast<int>(dpi), 96); }
} // namespace

Overlay::Overlay(OverlayGestures actions) : actions_(std::move(actions)) {}
Overlay::~Overlay() {
    if (window_) DestroyWindow(window_);
}

RECT Overlay::card_rect() const {
    const int x = right_ ? 184 : 20;
    return {pixels(x, dpi_), pixels(36, dpi_), pixels(x + 112, dpi_), pixels(108, dpi_)};
}

RECT Overlay::card_screen_rect() const {
    RECT rect = card_rect();
    MapWindowPoints(window_, nullptr, reinterpret_cast<POINT*>(&rect), 2);
    return rect;
}

bool Overlay::update_region() {
    const RECT r = card_rect();
    HRGN region = CreateRectRgn(r.left, r.top, r.right, r.bottom);
    if (!region) return false;
    // On success ownership of region transfers to the window manager.
    if (!SetWindowRgn(window_, region, TRUE)) {
        DeleteObject(region);
        return false;
    }
    InvalidateRect(window_, nullptr, FALSE);
    return true;
}

bool Overlay::open(POINT origin) {
    if (window_) return false;
    WNDCLASSW wc{};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = class_name;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    if (!CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                         class_name, L"ClipRope probe card", WS_POPUP,
                         origin.x, origin.y, host_width, host_height,
                         nullptr, nullptr, wc.hInstance, this)) return false;
    dpi_ = GetDpiForWindow(window_);
    if (!SetWindowPos(window_, nullptr, origin.x, origin.y, pixels(host_width, dpi_),
                      pixels(host_height, dpi_), SWP_NOACTIVATE | SWP_NOZORDER) ||
        !update_region()) {
        DestroyWindow(window_);
        return false;
    }
    ShowWindow(window_, SW_SHOWNOACTIVATE);
    UpdateWindow(window_);
    return true;
}

bool Overlay::set_right_slot(bool right) {
    const bool previous = right_;
    right_ = right;
    if (update_region()) return true;
    right_ = previous;
    return false;
}

bool Overlay::move_to(POINT origin) {
    return SetWindowPos(window_, nullptr, origin.x, origin.y, 0, 0,
                        SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
}

LRESULT CALLBACK Overlay::proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<Overlay*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = hwnd;
    }
    if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        rendering::paint_probe_card(dc, self->card_rect());
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DPICHANGED: {
        self->dpi_ = HIWORD(wp);
        const RECT suggested = *reinterpret_cast<const RECT*>(lp);
        SetWindowPos(hwnd, nullptr, suggested.left, suggested.top,
                     pixels(host_width, self->dpi_), pixels(host_height, self->dpi_),
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // A stale old hit region must never intercept the new gaps.
        if (!self->update_region()) ShowWindow(hwnd, SW_HIDE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (self->actions_.on_press) self->actions_.on_press();
        RECT host{};
        if (GetWindowRect(hwnd, &host) && GetCursorPos(&self->pressed_at_)) {
            self->offset_dip_ = {MulDiv(self->pressed_at_.x - host.left, 96, self->dpi_),
                                 MulDiv(self->pressed_at_.y - host.top, 96, self->dpi_)};
            SetCapture(hwnd);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (GetCapture() == hwnd && (wp & MK_LBUTTON)) {
            POINT cursor{};
            if (!GetCursorPos(&cursor)) return 0;
            if (!self->dragging_) {
                self->dragging_ = std::abs(cursor.x - self->pressed_at_.x) >=
                                      GetSystemMetricsForDpi(SM_CXDRAG, self->dpi_) ||
                                  std::abs(cursor.y - self->pressed_at_.y) >=
                                      GetSystemMetricsForDpi(SM_CYDRAG, self->dpi_);
            }
            if (self->dragging_) {
                self->move_to({cursor.x - pixels(self->offset_dip_.x, self->dpi_),
                               cursor.y - pixels(self->offset_dip_.y, self->dpi_)});
            }
        }
        return 0;
    case WM_LBUTTONDBLCLK:
        if (self->actions_.on_double_click) self->actions_.on_double_click();
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        self->dragging_ = false;
        return 0;
    case WM_NCDESTROY:
        self->window_ = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace cliprope::win
