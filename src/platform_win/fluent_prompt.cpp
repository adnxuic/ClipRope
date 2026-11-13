#include "platform_win/fluent.hpp"
#include <algorithm>

namespace cliprope::win {
namespace {
constexpr int heading_id = 1710, context_id = 1711, message_id = 1712;
constexpr int width_dip = 460;

struct Prompt {
    fluent::Surface theme;
    UiText title, context, message, action;
    bool confirm;
    int height;
};

void fit(HWND h, const Prompt& prompt, bool initial) {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(GetWindow(h, GW_OWNER), MONITOR_DEFAULTTOPRIMARY), &monitor);
    const auto& work = monitor.rcWork;
    const int width = std::min<LONG>(fluent::px(h, width_dip), work.right - work.left);
    RECT measured{0, 0, width - fluent::px(h, 50), 0};
    HDC dc = GetDC(h);
    const auto previous = SelectObject(dc, prompt.theme.get(fluent::Font::body));
    DrawTextW(dc, prompt.message.c_str(), -1, &measured, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(dc, previous);
    ReleaseDC(h, dc);
    const int height = std::min<LONG>(work.bottom - work.top,
        std::max<LONG>(fluent::px(h, prompt.height), measured.bottom + fluent::px(h, 139)));
    RECT current{};
    GetWindowRect(h, &current);
    const int x = initial ? (work.left + work.right - width) / 2 : std::clamp<int>(current.left, work.left, work.right-width);
    const int y = initial ? (work.top + work.bottom - height) / 2 : std::clamp<int>(current.top, work.top, work.bottom-height);
    SetWindowPos(h, nullptr, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER);
}

void layout(HWND h) {
    RECT client{};
    GetClientRect(h, &client);
    const int width = fluent::dip(h, client.right), button_y = fluent::dip(h, client.bottom) - 52;
    fluent::place(GetDlgItem(h, heading_id), 24, 14, width - 76, 34);
    fluent::place(GetDlgItem(h, context_id), 25, 49, width - 50, 18);
    fluent::place(GetDlgItem(h, message_id), 25, 77, width - 50, button_y - 83);
    const bool confirmation = GetDlgItem(h, IDCANCEL) != nullptr;
    fluent::place(GetDlgItem(h, IDOK), width - (confirmation ? 234 : 112), button_y,
                  confirmation ? 110 : 88, 34);
    fluent::place(GetDlgItem(h, IDCANCEL), width - 112, button_y, 88, 34);
}

INT_PTR CALLBACK prompt_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* prompt = reinterpret_cast<Prompt*>(GetWindowLongPtrW(h, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        prompt = reinterpret_cast<Prompt*>(l);
        SetWindowLongPtrW(h, DWLP_USER, l);
        auto& theme = prompt->theme;
        const bool confirm = prompt->confirm;
        const auto title = prompt->title;
        set_ui_text(h, title);
        auto close = theme.dialog_chrome(h);
        auto heading = fluent::child(h, L"STATIC", title, SS_NOPREFIX, heading_id);
        auto context = fluent::child(h, L"STATIC", prompt->context, SS_NOPREFIX | SS_ENDELLIPSIS, context_id);
        auto body = fluent::child(h, L"STATIC", prompt->message, SS_NOPREFIX, message_id);
        const auto action_label = prompt->action;
        auto action = fluent::child(h, L"BUTTON", action_label,
                                    WS_TABSTOP | (confirm ? BS_PUSHBUTTON | BS_NOTIFY : BS_DEFPUSHBUTTON), IDOK);
        auto cancel = confirm ? fluent::child(h, L"BUTTON", tr(TextKey::common_cancel), WS_TABSTOP | BS_DEFPUSHBUTTON, IDCANCEL) : nullptr;
        if (!close || !heading || !context || !body || !action || (confirm && !cancel)) {
            EndDialog(h, IDCANCEL);
            return TRUE;
        }
        theme.font(heading, fluent::Font::title);
        theme.font(context, fluent::Font::detail);
        theme.font(body);
        theme.card_style(action, {fluent::CardPart::dialog_button, 0, action_label, {}, {}, COLORREF(confirm), true});
        if (cancel) theme.card_style(cancel, {fluent::CardPart::dialog_button, 0, tr(TextKey::common_cancel)});
        SendMessageW(h, DM_SETDEFID, confirm ? IDCANCEL : IDOK, 0);
        fit(h, *prompt, true);
        ShowWindow(h, SW_SHOW);
        SetForegroundWindow(h);
        SetFocus(cancel ? cancel : action);
        return FALSE;
    }
    if (!prompt) return FALSE;
    switch (msg) {
    case WM_SIZE:
        layout(h);
        return TRUE;
    case WM_DPICHANGED: {
        prompt->theme.refresh();
        const auto rect = reinterpret_cast<const RECT*>(l);
        SetWindowPos(h, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        return TRUE;
    }
    case language_changed:
        fit(h, *prompt, false);
        layout(h);
        return TRUE;
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        prompt->theme.refresh();
        return TRUE;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<INT_PTR>(prompt->theme.background());
    case WM_CTLCOLORSTATIC:
        return prompt->theme.control_color(HDC(w), HWND(l));
    case WM_COMMAND:
        if (prompt->confirm && LOWORD(w) == IDOK &&
            (HIWORD(w) == BN_SETFOCUS || HIWORD(w) == BN_KILLFOCUS)) {
            SendMessageW(h, DM_SETDEFID, HIWORD(w) == BN_SETFOCUS ? IDOK : IDCANCEL, 0);
            return TRUE;
        }
        if (HIWORD(w) != BN_CLICKED || (LOWORD(w) != IDOK && LOWORD(w) != IDCANCEL)) return FALSE;
        EndDialog(h, LOWORD(w));
        return TRUE;
    case WM_CLOSE:
        EndDialog(h, IDCANCEL);
        return TRUE;
    }
    return FALSE;
}
}

INT_PTR fluent::modal(HWND owner, DLGPROC proc, LPARAM data) {
    // Native modal ownership and keyboard routing, with the same Fluent controls as the rope dialogs.
    struct alignas(DWORD) {
        DLGTEMPLATE dialog{WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME | DS_CENTER,
                           WS_EX_TOPMOST | WS_EX_TOOLWINDOW, 0, 0, 0, 220, 90};
        WORD menu{}, window_class{}, title{};
    } resource;
    return DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &resource.dialog, owner, proc, data);
}

bool fluent::management_prompt(HWND owner, UiText title, UiText context, UiText message,
                               UiText action, bool confirm, int height) {
    Prompt prompt{{}, title, context, message, action, confirm, height};
    const auto choice = modal(owner, prompt_proc, reinterpret_cast<LPARAM>(&prompt));
    return confirm && choice == IDOK;
}
} // namespace cliprope::win
