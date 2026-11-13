#include "platform_win/card_drag.hpp"
#include "presentation/card_labels.hpp"
#include <windowsx.h>
#include <shellscalingapi.h>
#include <cmath>
#include <stdexcept>

namespace cliprope::win {
namespace {
constexpr auto input_class = L"ClipRope.DragInput", visual_class = L"ClipRope.DragCard";
// Keep keyboard focus in the user's application. A background HWND can receive
// captured mouse messages over its visible surface, including other monitors.
LRESULT CALLBACK input_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    const auto owner = GetWindow(hwnd, GW_OWNER);
    switch (msg) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (SendMessageW(owner, WM_SETCURSOR, wp, lp)) return TRUE;
        break;
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP: {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        MapWindowPoints(hwnd, owner, &pt, 1);
        return SendMessageW(owner, msg, wp, MAKELPARAM(pt.x, pt.y));
    }
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        return SendMessageW(owner, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
void register_surface(const wchar_t *name, WNDPROC proc) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = name;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Could not register drag surface");
}
}

CardDrag::~CardDrag() { close(); }
void CardDrag::retranslate(const localization::LocalizationManager& labels) {
    presentation::localize_card(card, labels);
    const auto angle = painted_angle_;
    refresh_face();
    if (visual_ && monitor_dpi_) place(hook_, monitor_dpi_, angle);
}
void CardDrag::capture(HWND owner) {
    register_surface(input_class, input_proc);
    const DWORD style = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
    const auto instance = GetModuleHandleW(nullptr);
    input_ = CreateWindowExW(style, input_class, nullptr, WS_POPUP, GetSystemMetrics(SM_XVIRTUALSCREEN),
                             GetSystemMetrics(SM_YVIRTUALSCREEN), GetSystemMetrics(SM_CXVIRTUALSCREEN),
                             GetSystemMetrics(SM_CYVIRTUALSCREEN), owner, nullptr, instance, nullptr);
    // Alpha 1 keeps the input surface hit-testable in gaps and outside the rope.
    if (!input_ || !SetLayeredWindowAttributes(input_, 0, 1, LWA_ALPHA))
        throw std::runtime_error("Could not create drag input surface");
    ShowWindow(input_, SW_SHOWNOACTIVATE);
    SetCapture(input_);
    if (GetCapture() != input_)
        throw std::runtime_error("Could not capture drag input");
}
void CardDrag::open_visual(HWND owner) {
    register_surface(visual_class, DefWindowProcW);
    const DWORD style = WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED;
    visual_ = CreateWindowExW(style | WS_EX_TRANSPARENT, visual_class, nullptr, WS_POPUP,
                              0, 0, 1, 1, owner, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!visual_)
        throw std::runtime_error("Could not create dragged card");
}
void CardDrag::release_input() {
    if (input_) {
        if (GetCapture() == input_)
            ReleaseCapture();
        DestroyWindow(input_);
        input_ = nullptr;
    }
}
void CardDrag::close() {
    release_input();
    if (visual_) {
        DestroyWindow(visual_);
        visual_ = nullptr;
    }
    painted_dpi_ = 0;
    monitor_ = nullptr;
    card = {};
}
void CardDrag::follow(POINT cursor, float angle) {
    const auto monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    if (monitor != monitor_) {
        UINT dy{};
        if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &monitor_dpi_, &dy)))
            throw std::runtime_error("Drag monitor DPI unavailable");
        monitor_ = monitor;
    }
    const auto hook = presentation::dragged_hook({float(cursor.x), float(cursor.y)}, monitor_dpi_ / 96.f, grip, angle);
    place(hook, monitor_dpi_, angle);
}
void CardDrag::place(presentation::PointF hook, UINT dpi, float angle) {
    // Padding contains the sheet and its shadow over the full swing range.
    const auto& g = geometry;
    const presentation::CardFrame local{g.width / 2 + g.carried_padding(), 24, angle};
    const float scale = dpi / 96.f;
    // Round half pixels in the same direction on either side of the desktop origin.
    POINT origin{LONG(std::floor(hook.x - local.hook_x * scale + 0.5)),
                 LONG(std::floor(hook.y - local.rope * scale + 0.5))};
    hook_ = hook;
    if (painted_dpi_ == dpi && painted_angle_ == angle) {
        if ((origin.x != origin_.x || origin.y != origin_.y) &&
            !SetWindowPos(visual_, nullptr, origin.x, origin.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE))
            throw std::runtime_error("Could not move dragged card");
    } else {
        const RECT bounds{0, 0, LONG(std::ceil(2 * local.hook_x * scale)), LONG(std::ceil(g.carried_height() * scale))};
        const auto paint = rendering::WorkspacePaint::acquire();
        if (!rendering::paint_window(visual_, nullptr, bounds, {}, [&](HDC dc) {
                return paint->draw_carried(dc, bounds, dpi, card, rope, index, g, local, colors);
            }, &origin))
            throw std::runtime_error("Could not paint dragged card");
        if (!IsWindowVisible(visual_))
            ShowWindow(visual_, SW_SHOWNOACTIVATE);
        painted_dpi_ = dpi;
        painted_angle_ = angle;
    }
    origin_ = origin;
}
} // namespace cliprope::win
