#include "platform_win/uninstall_dialog.hpp"
#include "platform_win/fluent.hpp"
#include "platform_win/fluent_tooltip.hpp"
#include "platform_win/app_icon.hpp"
#include <algorithm>
#include <chrono>
#include <future>

namespace cliprope::win {
using namespace fluent;
namespace {
constexpr int heading_id = 2452, path_id = 2453, intro_id = 2454, history_label_id = 2455, history_detail_id = 2456;
constexpr UINT_PTR pending_timer = 2457;
struct Dialog {
    Surface surface;
    Tooltip tooltip;
    UninstallActions actions;
    BuildUninstallPreview preview;
    std::future<BuildUninstallResult> pending;
    bool completed{}, failed{};
};
UiText failure_text(const BuildUninstallResult& result) {
    switch (result.issue) {
    case UninstallIssue::layout: return tr(TextKey::uninstall_invalid_layout);
    case UninstallIssue::running: return tr(TextKey::uninstall_running);
    case UninstallIssue::shared_history: return tr(TextKey::uninstall_shared_history);
    case UninstallIssue::registry: return tr(TextKey::uninstall_registry_failed);
    case UninstallIssue::history: return tr(TextKey::uninstall_history_failed);
    case UninstallIssue::preview: return tr(TextKey::uninstall_preview_failed);
    case UninstallIssue::payload:
        return message(TextKey::uninstall_file_failed,
                       {{"file", std::filesystem::path(result.detail).filename().wstring()},
                        {"code", std::to_wstring(result.windows_error)}});
    default: return tr(TextKey::uninstall_failed);
    }
}
void status(HWND h, UiText text, bool error) {
    auto& dialog = *reinterpret_cast<Dialog*>(GetWindowLongPtrW(h, DWLP_USER));
    dialog.failed = error;
    const auto control = GetDlgItem(h, uninstall_status_id);
    set_ui_text(control, text);
    accessible_name(control, text);
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, control, OBJID_CLIENT, CHILDID_SELF);
    NotifyWinEvent(EVENT_SYSTEM_ALERT, control, OBJID_CLIENT, CHILDID_SELF);
    InvalidateRect(control, nullptr, FALSE);
}
void inspect(HWND h) {
    auto& dialog = *reinterpret_cast<Dialog*>(GetWindowLongPtrW(h, DWLP_USER));
    if (dialog.pending.valid() || dialog.completed) return;
    const auto previous_issue = dialog.preview.issue;
    try { dialog.preview = dialog.actions.inspect(); }
    catch (const std::exception&) { dialog.preview.issue = UninstallIssue::layout; }
    set_ui_text(GetDlgItem(h, path_id), dialog.preview.directory.wstring());
    const auto toggle = GetDlgItem(h, uninstall_history_id);
    EnableWindow(toggle, dialog.preview.delete_history_available);
    if (!dialog.preview.delete_history_available) SendMessageW(toggle, BM_SETCHECK, BST_UNCHECKED, 0);
    const auto detail = !dialog.preview.delete_history_available && dialog.preview.issue == UninstallIssue::none
        ? tr(TextKey::uninstall_shared_history) : tr(TextKey::uninstall_history_description);
    set_ui_text(GetDlgItem(h, history_detail_id), detail);
    accessible_description(toggle, detail);
    if (dialog.preview.issue != UninstallIssue::none) {
        const BuildUninstallResult result{false, false, dialog.preview.issue, dialog.preview.detail, dialog.preview.windows_error};
        status(h, failure_text(result), true);
    } else if (previous_issue != UninstallIssue::none) status(h,{},false);
}
void layout(HWND h) {
    RECT bounds{}; GetClientRect(h, &bounds);
    const int w = dip(h, bounds.right), height = dip(h, bounds.bottom);
    place(GetDlgItem(h, heading_id), 24, 14, w - 72, 38);
    place(GetDlgItem(h, path_id), 25, 59, w - 50, 22);
    place(GetDlgItem(h, intro_id), 25, 95, w - 50, 51);
    place(GetDlgItem(h, history_label_id), 40, 159, w - 135, 28);
    place(GetDlgItem(h, uninstall_history_id), w - 81, 157, 42, 32);
    place(GetDlgItem(h, history_detail_id), 40, 193, w - 80, 43);
    place(GetDlgItem(h, uninstall_status_id), 25, 248, w - 50, std::max(38, height - 310));
    auto& dialog = *reinterpret_cast<Dialog*>(GetWindowLongPtrW(h, DWLP_USER));
    place(GetDlgItem(h, IDOK), w - (dialog.completed ? 112 : 234), height - 52, dialog.completed ? 88 : 110, 34);
    place(GetDlgItem(h, IDCANCEL), w - 112, height - 52, 88, 34);
    InvalidateRect(h,nullptr,FALSE);
}
void paint(HWND h, HDC dc) {
    auto& dialog = *reinterpret_cast<Dialog*>(GetWindowLongPtrW(h, DWLP_USER));
    RECT bounds{}; GetClientRect(h,&bounds);
    FillRect(dc,&bounds,dialog.surface.background());
    const RECT panel{px(h,24),px(h,149),bounds.right-px(h,24),px(h,241)};
    dialog.surface.draw_card(h,dc,panel,{CardPart::panel});
}
void fit(HWND h) {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const auto& work = monitor.rcWork;
    const int width = std::min<LONG>(px(h, 560), work.right - work.left);
    const int height = std::min<LONG>(px(h, 400), work.bottom - work.top);
    SetWindowPos(h, nullptr, work.left + (work.right-work.left-width)/2, work.top + (work.bottom-work.top-height)/2,
                 width, height, SWP_NOACTIVATE | SWP_NOZORDER);
}
void pending_state(HWND h, bool busy) {
    auto& dialog = *reinterpret_cast<Dialog*>(GetWindowLongPtrW(h, DWLP_USER));
    dialog.tooltip.hide();
    EnableWindow(GetDlgItem(h, IDOK), !busy);
    EnableWindow(GetDlgItem(h, IDCANCEL), !busy);
    EnableWindow(GetDlgItem(h, dialog_close_id), !busy);
    EnableWindow(GetDlgItem(h, uninstall_history_id), !busy && dialog.preview.delete_history_available);
}
void finish(HWND h, const BuildUninstallResult& result) {
    auto& dialog = *reinterpret_cast<Dialog*>(GetWindowLongPtrW(h, DWLP_USER));
    pending_state(h,false);
    if (result.success) {
        dialog.completed = true;
        status(h,tr(result.history_touched ? TextKey::uninstall_done_deleted : TextKey::uninstall_done_kept),false);
        set_ui_text(GetDlgItem(h,IDOK),tr(TextKey::common_close));
        dialog.surface.card_style(GetDlgItem(h,IDOK),{CardPart::dialog_button,0,tr(TextKey::common_close)});
        EnableWindow(GetDlgItem(h,uninstall_history_id),FALSE);
        ShowWindow(GetDlgItem(h,IDCANCEL),SW_HIDE);
        layout(h); SendMessageW(h,DM_SETDEFID,IDOK,0); SetFocus(GetDlgItem(h,IDOK));
    } else { inspect(h); status(h,failure_text(result),true); SetFocus(GetDlgItem(h,IDOK)); }
}
INT_PTR CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(h, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        dialog = reinterpret_cast<Dialog*>(l);
        SetWindowLongPtrW(h, DWLP_USER, l);
        set_ui_text(h, tr(TextKey::uninstall_title));
        if (!dialog->surface.dialog_chrome(h)) { EndDialog(h, IDCANCEL); return TRUE; }
        set_application_icons(h);
        for (const auto& [id, text] : std::initializer_list<std::pair<int, UiText>>{
                 {heading_id,tr(TextKey::uninstall_title)}, {path_id,L""}, {intro_id,tr(TextKey::uninstall_description)},
                 {history_label_id,tr(TextKey::uninstall_history)}, {history_detail_id,L""}, {uninstall_status_id,L""}}) {
            auto label = child(h, L"STATIC", text, SS_NOPREFIX | (id == path_id ? SS_ENDELLIPSIS : SS_EDITCONTROL), id);
            if (!label) { EndDialog(h,IDCANCEL); return TRUE; }
            dialog->surface.font(label, id == heading_id ? Font::title : id == path_id || id == history_detail_id ? Font::detail : Font::body);
        }
        auto toggle = child(h,L"BUTTON",tr(TextKey::uninstall_history),WS_TABSTOP | BS_AUTOCHECKBOX | BS_NOTIFY,uninstall_history_id);
        dialog->surface.card_style(toggle,{CardPart::settings_switch});
        accessible_name(toggle,tr(TextKey::uninstall_history));
        const auto action = child(h,L"BUTTON",tr(TextKey::uninstall_action),WS_TABSTOP | BS_NOTIFY,IDOK);
        const auto cancel = child(h,L"BUTTON",tr(TextKey::common_cancel),WS_TABSTOP | BS_NOTIFY | BS_DEFPUSHBUTTON,IDCANCEL);
        if (!toggle || !action || !cancel) { EndDialog(h,IDCANCEL); return TRUE; }
        dialog->surface.card_style(action,{CardPart::dialog_button,0,tr(TextKey::uninstall_action),{},{},1,true});
        dialog->surface.card_style(cancel,{CardPart::dialog_button,0,tr(TextKey::common_cancel)});
        dialog->surface.font(action); dialog->surface.font(cancel);
        SendMessageW(h,DM_SETDEFID,IDCANCEL,0);
        inspect(h); fit(h); layout(h);
        dialog->tooltip.bind(h,GetDlgItem(h,path_id),[dialog] { return UiText(dialog->preview.directory.wstring()); });
        ShowWindow(h,SW_SHOW); SetForegroundWindow(h); SetFocus(cancel);
        return FALSE;
    }
    if (!dialog) return FALSE;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{}; const auto dc = BeginPaint(h,&ps);
        paint(h,dc); EndPaint(h,&ps); return TRUE;
    }
    case WM_PRINTCLIENT: paint(h,HDC(w)); return TRUE;
    case WM_SIZE: layout(h); return TRUE;
    case WM_ACTIVATE:
        if (LOWORD(w) != WA_INACTIVE) inspect(h);
        return FALSE;
    case WM_DPICHANGED: {
        dialog->tooltip.hide(); dialog->surface.refresh(); set_application_icons(h,HIWORD(w));
        const auto rect = reinterpret_cast<const RECT*>(l);
        SetWindowPos(h,nullptr,rect->left,rect->top,rect->right-rect->left,rect->bottom-rect->top,SWP_NOACTIVATE | SWP_NOZORDER);
        layout(h); return TRUE;
    }
    case WM_SETTINGCHANGE: case WM_THEMECHANGED: case WM_SYSCOLORCHANGE: case language_changed:
        dialog->surface.refresh(); layout(h); return TRUE;
    case WM_CTLCOLORDLG: return reinterpret_cast<INT_PTR>(dialog->surface.background());
    case WM_CTLCOLORSTATIC: {
        const int id = GetDlgCtrlID(HWND(l));
        const auto brush = dialog->surface.control_color(HDC(w),HWND(l),id == history_label_id || id == history_detail_id);
        if (id == history_detail_id || (id == history_label_id && !IsWindowEnabled(GetDlgItem(h,uninstall_history_id))))
            SetTextColor(HDC(w),dialog->surface.colors.muted);
        if (HWND(l) == GetDlgItem(h,uninstall_status_id) && dialog->failed) SetTextColor(HDC(w),dialog->surface.colors.danger);
        return brush;
    }
    case WM_COMMAND:
        if (LOWORD(w) == IDOK && (HIWORD(w) == BN_SETFOCUS || HIWORD(w) == BN_KILLFOCUS)) {
            SendMessageW(h,DM_SETDEFID,HIWORD(w) == BN_SETFOCUS ? IDOK : IDCANCEL,0); return TRUE;
        }
        if (HIWORD(w) != BN_CLICKED || dialog->pending.valid()) return FALSE;
        if (LOWORD(w) == IDCANCEL) { dialog->tooltip.clear(); EndDialog(h,dialog->completed ? IDOK : IDCANCEL); return TRUE; }
        if (LOWORD(w) != IDOK) return FALSE;
        if (dialog->completed) { EndDialog(h,IDOK); return TRUE; }
        {
            const bool erase = IsWindowEnabled(GetDlgItem(h,uninstall_history_id)) &&
                SendMessageW(GetDlgItem(h,uninstall_history_id),BM_GETCHECK,0,0) == BST_CHECKED;
            status(h,tr(TextKey::uninstall_pending),false);
            pending_state(h,true);
            try {
                dialog->pending = std::async(std::launch::async,[remove=dialog->actions.remove,erase] { return remove(erase); });
                if (!SetTimer(h,pending_timer,30,nullptr)) finish(h,dialog->pending.get());
            } catch (const std::exception&) { pending_state(h,false); status(h,tr(TextKey::uninstall_failed),true); }
        }
        return TRUE;
    case WM_TIMER:
        if (w == pending_timer && dialog->pending.valid() &&
            dialog->pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
            KillTimer(h,pending_timer);
            BuildUninstallResult result;
            try { result = dialog->pending.get(); } catch (const std::exception&) { result.issue = UninstallIssue::none; }
            finish(h,result);
            return TRUE;
        }
        return FALSE;
    case WM_CLOSE:
        if (!dialog->pending.valid()) { dialog->tooltip.clear(); EndDialog(h,dialog->completed ? IDOK : IDCANCEL); }
        return TRUE;
    case WM_NCDESTROY: KillTimer(h,pending_timer); dialog->tooltip.clear(); return FALSE;
    }
    return FALSE;
}
} // namespace
bool uninstall_dialog(HWND owner, UninstallActions actions) {
    Dialog dialog;
    dialog.actions = std::move(actions);
    return fluent::modal(owner,proc,reinterpret_cast<LPARAM>(&dialog)) == IDOK && dialog.completed;
}
} // namespace cliprope::win
