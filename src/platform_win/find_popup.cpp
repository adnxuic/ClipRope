#include "platform_win/find_popup.hpp"
#include <imm.h>
#include <commctrl.h>
#include <algorithm>
#include <stdexcept>

namespace cliprope::win {
using namespace fluent;
void FindPopup::open(HWND owner, POINT anchor, UiText scope) {
    const HWND previous = GetForegroundWindow();
    if (!hwnd_) {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = proc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"ClipRope.FindInCards";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            throw std::runtime_error("Could not register Find in Cards");
        focus_.emplace();
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &monitor);
        const int dpi = design_dpi(GetDpiForWindow(owner));
        const int w = MulDiv(372, dpi, 96), h = MulDiv(76, dpi, 96);
        const auto area = monitor.rcWork;
        const int x = std::clamp(anchor.x, area.left, std::max(area.left, area.right - w));
        const int y = std::clamp(anchor.y + 6, area.top, std::max(area.top, area.bottom - h));
        if (!create_ui_window(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, tr(TextKey::find_title),
                             WS_POPUP | WS_CLIPCHILDREN, x, y, w, h, owner, nullptr, wc.hInstance, this))
            throw std::runtime_error("Could not open Find in Cards");
        status(L"", scope, false);
        BOOL animate{};
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
        if (!animate || !AnimateWindow(hwnd_, 100, AW_BLEND)) ShowWindow(hwnd_, SW_SHOW);
    }
    if (!SetForegroundWindow(hwnd_) && previous && GetForegroundWindow() == previous && !IsHungAppWindow(previous)) {
        const auto caller = GetCurrentThreadId(), foreground = GetWindowThreadProcessId(previous, nullptr);
        // A scoped hover press can start on a different input queue. Join only
        // that foreground window for the explicit handoff, never a later one.
        if (foreground != caller && AttachThreadInput(caller, foreground, TRUE)) {
            if (GetForegroundWindow() == previous) SetForegroundWindow(hwnd_);
            if (GetForegroundWindow() == hwnd_) SetFocus(edit_);
            AttachThreadInput(caller, foreground, FALSE);
        }
    }
    if (GetForegroundWindow() == hwnd_) SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
}
void FindPopup::close(bool restore) {
    if (!hwnd_) return;
    if (restore && focus_) focus_->restore(hwnd_, GetWindow(hwnd_, GW_OWNER));
    DestroyWindow(hwnd_);
    focus_.reset();
    if (closed) closed();
}
void FindPopup::status(UiText count, UiText scope, bool navigable) {
    if (!hwnd_) return;
    set_ui_text(count_, count);
    set_ui_text(scope_, scope);
    EnableWindow(prev_, navigable);
    EnableWindow(next_, navigable);
}
void FindPopup::layout() {
    place(edit_, 33, 8, 147, 36);
    place(count_, 182, 9, 85, 32);
    place(prev_, 269, 9, 30, 30);
    place(next_, 301, 9, 30, 30);
    place(close_, 333, 9, 30, 30);
    place(scope_, 12, 47, 348, 22);
    RECT r{};
    GetClientRect(hwnd_, &r);
    auto region = CreateRoundRectRgn(0, 0, r.right + 1, r.bottom + 1, px(hwnd_, 24), px(hwnd_, 24));
    if (region && !SetWindowRgn(hwnd_, region, TRUE)) DeleteObject(region);
}
void FindPopup::paint(HDC dc) {
    RECT r{};
    GetClientRect(hwnd_, &r);
    auto& colors = surface_.colors;
    round_box(dc, r, px(hwnd_, 12), colors.panel, colors.line);
    RECT icon{px(hwnd_, 10), px(hwnd_, 9), px(hwnd_, 31), px(hwnd_, 40)};
    surface_.draw_card(hwnd_, dc, icon, {CardPart::icon_button, Icon::search});
    RECT divider{px(hwnd_, 266), px(hwnd_, 13), px(hwnd_, 267), px(hwnd_, 36)};
    solid(dc, divider, colors.line);
}
bool FindPopup::translate(const MSG& msg) {
    if (!hwnd_ || (msg.hwnd != hwnd_ && !IsChild(hwnd_, msg.hwnd))) return false;
    if (msg.message != WM_KEYDOWN) return false;
    if (GetFocus() == edit_) {
        HIMC imc = ImmGetContext(edit_);
        const bool composing = imc && ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0) > 0;
        if (imc) ImmReleaseContext(edit_, imc);
        if (composing_ || composing) return false;
    }
    if (msg.wParam == VK_ESCAPE)
        close(true);
    else if (msg.wParam == 'F' && GetKeyState(VK_CONTROL) < 0) {
        SetFocus(edit_);
        SendMessageW(edit_, EM_SETSEL, 0, -1);
    } else if (msg.wParam == VK_RETURN && GetFocus() != edit_) {
        if (IsWindowEnabled(GetFocus())) SendMessageW(GetFocus(), BM_CLICK, 0, 0);
    } else if (msg.wParam == VK_TAB)
        return IsDialogMessageW(hwnd_, const_cast<MSG*>(&msg)) != FALSE;
    else if (msg.wParam == VK_UP || msg.wParam == VK_DOWN || (msg.wParam == VK_RETURN && GetFocus() == edit_)) {
        if (navigate && IsWindowEnabled(next_))
            navigate(msg.wParam == VK_UP || (msg.wParam == VK_RETURN && GetKeyState(VK_SHIFT) < 0) ? -1 : 1);
    } else
        return false;
    return true;
}
LRESULT CALLBACK FindPopup::edit_proc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    auto& self = *reinterpret_cast<FindPopup*>(data);
    // A single-line EDIT draws at the top of its client rectangle. Keep its
    // full hit target and center the native text on the same line as the count.
    if (msg == WM_NCCALCSIZE) {
        auto result = DefSubclassProc(h, msg, w, l);
        auto& rect = w ? reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0] : *reinterpret_cast<RECT*>(l);
        const int pad = std::min<int>(px(h, 7), std::max<LONG>(0, (rect.bottom - rect.top - 1) / 2));
        rect.top += pad;
        rect.bottom -= pad;
        return result;
    }
    if (msg == WM_NCPAINT) {
        auto dc = GetWindowDC(h);
        if (dc) {
            RECT rect{};
            GetWindowRect(h, &rect);
            RECT client{};
            GetClientRect(h, &client);
            MapWindowPoints(h, nullptr, reinterpret_cast<POINT*>(&client), 2);
            OffsetRect(&client, -rect.left, -rect.top);
            OffsetRect(&rect, -rect.left, -rect.top);
            ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
            solid(dc, rect, self.surface_.colors.panel);
            ReleaseDC(h, dc);
        }
        return 0;
    }
    if (msg == WM_NCHITTEST) return HTCLIENT;
    if (msg == WM_IME_STARTCOMPOSITION) self.composing_ = true;
    if (msg == WM_IME_ENDCOMPOSITION || msg == WM_NCDESTROY) self.composing_ = false;
    return DefSubclassProc(h, msg, w, l);
}
LRESULT CALLBACK FindPopup::proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<FindPopup*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<FindPopup*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(self));
        self->hwnd_ = h;
    }
    if (!self) return DefWindowProcW(h, msg, w, l);
    switch (msg) {
    case WM_CREATE: {
        self->surface_.attach(h);
        self->edit_ = child(h, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 1);
        accessible_name(self->edit_, tr(TextKey::find_title));
        SetWindowSubclass(self->edit_, edit_proc, 1, reinterpret_cast<DWORD_PTR>(self));
        SendMessageW(self->edit_, EM_SETCUEBANNER, TRUE, LPARAM(ui_text(TextKey::find_title)));
        SendMessageW(self->edit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(4, 4));
        self->count_ = child(h, L"STATIC", L"", SS_RIGHT | SS_CENTERIMAGE, 2);
        self->scope_ = child(h, L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, 3);
        auto button = [&](UiText name, wchar_t glyph, int id) {
            auto b = child(h, L"BUTTON", name, WS_TABSTOP | BS_PUSHBUTTON, id);
            self->surface_.card_style(b, {CardPart::find_button, glyph, name});
            accessible_name(b, name);
            return b;
        };
        self->prev_ = button(tr(TextKey::find_previous), Icon::chevron_up, 4);
        self->next_ = button(tr(TextKey::find_next), Icon::chevron_down, 5);
        self->close_ = button(tr(TextKey::find_close), Icon::close, 6);
        for (auto c : {self->edit_, self->count_, self->scope_})
            self->surface_.font(c);
        self->layout();
        return 0;
    }
    case WM_DPICHANGED: {
        auto r = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        self->surface_.refresh();
        self->layout();
        return 0;
    }
    case WM_THEMECHANGED:
    case language_changed:
        SendMessageW(self->edit_, EM_SETCUEBANNER, TRUE, LPARAM(ui_text(TextKey::find_title)));
        self->layout();
        return 0;
    case WM_SETTINGCHANGE:
        self->surface_.refresh();
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        self->paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        self->paint(HDC(w));
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CTLCOLORSTATIC:
        SetTextColor(HDC(w), self->surface_.colors.muted);
        SetBkColor(HDC(w), self->surface_.colors.panel);
        return reinterpret_cast<LRESULT>(self->surface_.background(true));
    case WM_CTLCOLOREDIT:
        return self->surface_.control_color(HDC(w), HWND(l), true);
    case WM_COMMAND:
        if (LOWORD(w) == 1 && HIWORD(w) == EN_CHANGE && self->changed) self->changed(caption(self->edit_));
        if (HIWORD(w) == BN_CLICKED && LOWORD(w) >= 4 && LOWORD(w) <= 5 && self->navigate)
            self->navigate(LOWORD(w) == 4 ? -1 : 1);
        if (LOWORD(w) == 6 && HIWORD(w) == BN_CLICKED) self->close(true);
        return 0;
    case WM_CLOSE:
        self->close(true);
        return 0;
    case WM_NCDESTROY:
        self->hwnd_ = nullptr;
        self->surface_.release();
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}
} // namespace cliprope::win
