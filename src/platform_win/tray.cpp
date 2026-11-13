#include "platform_win/tray.hpp"
#include "platform_win/app_icon.hpp"
#include <shellapi.h>
#include "platform_win/popup_menu.hpp"
#include "platform_win/tray_popup.hpp"
#include "platform_win/menu_timing.hpp"
#include <windowsx.h>
#include <algorithm>

namespace cliprope::win {
namespace {
constexpr int first_hotkey=0x4260;
constexpr UINT_PTR icon_retry = 0xA275;
// Two IDs per global action allow the replacement to be acquired before releasing the old key.
int candidate_id(application::ShortcutAction action, int active) {
    const int base = first_hotkey + int(action);
    return active == base ? base + 0x10 : base;
}
NOTIFYICONDATAW icon_data(HWND owner) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd=owner;
    data.uID=Tray::icon_id;
    return data;
}
RECT notification_anchor(HWND listener, POINT event) {
    NOTIFYICONIDENTIFIER icon{sizeof(icon)};
    icon.hWnd = listener;
    icon.uID = Tray::icon_id;
    RECT bounds{};
    const bool located = Shell_NotifyIconGetRect(&icon, &bounds) == S_OK && !IsRectEmpty(&bounds);
    const bool keyboard = event.x == -1 && event.y == -1;
    if (located) {
        if (keyboard || PtInRect(&bounds, event)) return bounds;
        // Explorer can report its overflow button while the clicked icon is in the open flyout.
        const HWND root = GetAncestor(WindowFromPoint(event), GA_ROOT);
        wchar_t cls[64]{};
        GetClassNameW(root, cls, int(std::size(cls)));
        if (wcscmp(cls, L"TopLevelWindowForOverflowXamlIsland") && wcscmp(cls, L"NotifyIconOverflowWindow"))
            return bounds;
    }
    if (keyboard && !GetCursorPos(&event)) return {};
    return {event.x, event.y, event.x + 1, event.y + 1};
}
void return_notification_focus(HWND listener, int choice) {
    // The editor takes foreground immediately after the menu closes.
    if (choice == tray_command::quick_note) return;
    // Transfer foreground while the popup still owns it. Explorer can then focus
    // the icon in either the taskbar or its overflow before the popup is destroyed.
    HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr);
    DWORD shell_pid{}, taskbar_pid{};
    GetWindowThreadProcessId(GetShellWindow(), &shell_pid);
    GetWindowThreadProcessId(taskbar, &taskbar_pid);
    if (taskbar_pid && taskbar_pid == shell_pid) {
        AllowSetForegroundWindow(shell_pid);
        SetForegroundWindow(taskbar);
    }
    auto data = icon_data(listener);
    Shell_NotifyIconW(NIM_SETFOCUS, &data);
}
bool interactive_station() {
    USEROBJECTFLAGS flags{};
    DWORD used{};
    return GetUserObjectInformationW(GetProcessWindowStation(),UOI_FLAGS,&flags,sizeof(flags),&used)
        && (flags.dwFlags & WSF_VISIBLE);
}
std::wstring menu_text(std::wstring_view input) {
    std::wstring out;
    for (auto ch : input.substr(0,80)) {
        if (ch<32) ch=L' ';
        out+=ch;
    }
    if (!out.empty() && out.back()>=0xd800 && out.back()<=0xdbff) out.pop_back();
    return out;
}
}
Tray::Tray(TrayActions actions, decltype(&Shell_NotifyIconW) shell, std::optional<bool> visible_station)
    : actions_(std::move(actions)), shell_(shell), visible_station_(visible_station.value_or(interactive_station())) {}
Tray::~Tray() { stop(); }
bool Tray::start(HWND listener) {
    if (!IsWindow(listener) || listener_) return false;
    listener_=listener;
    shell_restart_=RegisterWindowMessageW(L"TaskbarCreated");
    recover_icon(false);
    reload_shortcuts();
    if (bindings_[0].enabled && !hotkey_registered() && interactive_station()) settings();
    return true;
}
void Tray::reload_shortcuts() {
    auto saved = actions_.read_shortcuts();
    for (auto& id : registrations_) {
        if (id) UnregisterHotKey(listener_, id);
        id = 0;
    }
    bindings_ = std::move(saved);
    for (auto action : {application::workspace_toggle, application::quick_note})
        if (bindings_[action].enabled) configure(action, std::nullopt);
    update_scoped();
}
void Tray::stop() {
    settings_.close();
    if (!listener_) return;
    KillTimer(listener_, icon_retry);
    for (auto& id : registrations_) {
        if (id) UnregisterHotKey(listener_, id);
        id = 0;
    }
    if (icon_added_) {
        auto data=icon_data(listener_);
        shell_(NIM_DELETE,&data);
    }
    listener_=nullptr; icon_added_=false;
}
void Tray::install_icon() {
    auto data=icon_data(listener_);
    data.uFlags=NIF_ICON|NIF_TIP|NIF_MESSAGE|NIF_SHOWTIP;
    data.uCallbackMessage=callback;
    data.hIcon=notification_icon();
    wcscpy_s(data.szTip,L"ClipRope");
    icon_added_=shell_(NIM_ADD,&data)!=FALSE;
    if (!icon_added_) icon_added_=shell_(NIM_MODIFY,&data)!=FALSE;
    if (icon_added_) {
        data.uVersion=NOTIFYICON_VERSION_4;
        if (!shell_(NIM_SETVERSION,&data)) {
            shell_(NIM_DELETE,&data);
            icon_added_=false;
        }
    }
}
void Tray::recover_icon(bool expose) {
    KillTimer(listener_, icon_retry);
    install_icon();
    if (icon_added_) { retries_ = 0; return; }
    constexpr UINT delays[]{250, 1000, 4000};
    if (visible_station_ && retries_ < std::size(delays)) SetTimer(listener_, icon_retry, delays[retries_++], nullptr);
    if (expose && needs_workspace() && actions_.open_workspace) actions_.open_workspace();
}
void Tray::notify(UiText text) const {
    if (!listener_ || !icon_added_) return;
    auto data=icon_data(listener_);
    data.uFlags=NIF_INFO;
    data.dwInfoFlags=NIIF_INFO;
    wcscpy_s(data.szInfoTitle,L"ClipRope");
    const auto length=std::min(text.size(),std::size(data.szInfo)-1);
    std::copy_n(text.begin(),length,data.szInfo);
    Shell_NotifyIconW(NIM_MODIFY,&data);
}
UiText Tray::conflict(application::ShortcutAction action, Hotkey chord, bool all) const {
    for (unsigned i = 0; i < (all ? unsigned(bindings_.size()) : unsigned(action)); ++i) {
        if (i != action && bindings_[i].enabled && parse_hotkey(bindings_[i].chord) == chord)
            return cliprope::win::message(TextKey::hotkey_used_by, {{"action", shortcut_labels[i]}});
    }
    return {};
}
void Tray::update_scoped() {
    ScopedKeys keys{};
    for (unsigned i = application::next_rope; i < bindings_.size(); ++i) {
        const auto action = static_cast<application::ShortcutAction>(i);
        auto chord = parse_hotkey(bindings_[i].chord);
        if (bindings_[i].enabled && chord && valid_hotkey(*chord, false) && conflict(action, *chord, false).empty())
            keys[i - application::next_rope] = *chord;
    }
    if (actions_.set_scoped) actions_.set_scoped(keys);
}
ShortcutResult Tray::status(application::ShortcutAction action) const {
    if (!bindings_[action].enabled) return {ShortcutState::off, tr(TextKey::common_off)};
    const auto chord = parse_hotkey(bindings_[action].chord);
    if (!chord || !valid_hotkey(*chord, global_shortcut(action))) return {ShortcutState::unavailable, tr(TextKey::hotkey_invalid_saved)};
    if (auto used = conflict(action, *chord, false); !used.empty()) return {ShortcutState::unavailable, used};
    if (global_shortcut(action))
        return registrations_[action] ? ShortcutResult{ShortcutState::registered, tr(TextKey::hotkey_registered)}
                                      : ShortcutResult{ShortcutState::unavailable, tr(TextKey::hotkey_registration_failed)};
    auto error = actions_.hover_status ? actions_.hover_status() : std::nullopt;
    if (!error) return {ShortcutState::unverified, tr(TextKey::hotkey_hover_pending)};
    if (*error) return {ShortcutState::limited, cliprope::win::message(TextKey::hotkey_hover_error, {{"code", std::to_wstring(*error)}})};
    return {ShortcutState::available, tr(TextKey::hotkey_hover_available)};
}
std::wstring Tray::shortcut_hint(application::ShortcutAction action) const {
    auto chord = parse_hotkey(bindings_[action].chord);
    if (!bindings_[action].enabled || !chord || !valid_hotkey(*chord, global_shortcut(action)) ||
        !conflict(action, *chord, false).empty() || (global_shortcut(action) && !registrations_[action])) return {};
    const auto name = hotkey_name(*chord);
    return {name.begin(), name.end()};
}
ShortcutResult Tray::configure(application::ShortcutAction action, std::optional<application::ShortcutBinding> candidate) {
    const auto& previous = bindings_[action];
    auto next = candidate.value_or(previous);
    const bool changed = next != previous;
    const bool global = global_shortcut(action);
    int old_id = global ? registrations_[action] : 0, acquired = 0;
    // A damaged or conflicting saved chord can always be switched off.
    const bool disable_only = changed && !next.enabled && next.chord == previous.chord;
    if (!disable_only) {
        const auto chord = parse_hotkey(next.chord);
        if (!chord || !valid_hotkey(*chord, global)) return {ShortcutState::unavailable, tr(TextKey::hotkey_invalid_chord)};
        if (auto used = conflict(action, *chord, changed || !next.enabled); !used.empty()) return {ShortcutState::unavailable, used};
        if (global && !(old_id && parse_hotkey(previous.chord) == chord)) {
            acquired = candidate_id(action, old_id);
            if (!RegisterHotKey(listener_, acquired, chord->modifiers | MOD_NOREPEAT, chord->key))
                return {ShortcutState::unavailable, tr(TextKey::hotkey_registration_failed)};
        }
    }
    if (changed) {
        try { actions_.save_shortcut(action, next); }
        catch (const std::exception&) {
            if (acquired) UnregisterHotKey(listener_, acquired);
            return {ShortcutState::save_failed, tr(TextKey::hotkey_save_failed)};
        }
        bindings_[action] = std::move(next);
    }
    if (global) {
        const bool enabled = bindings_[action].enabled;
        if (old_id && (acquired || !enabled)) UnregisterHotKey(listener_, old_id);
        if (acquired && !enabled) UnregisterHotKey(listener_, acquired);
        registrations_[action] = enabled ? (acquired ? acquired : old_id) : 0;
    }
    if (changed) update_scoped();
    if (!bindings_[action].enabled && !disable_only) {
        if (!global) {
            auto error = actions_.hover_status ? actions_.hover_status() : std::nullopt;
            if (!error) return {ShortcutState::unverified, tr(TextKey::hotkey_hover_off_pending)};
            if (*error) return {ShortcutState::limited, cliprope::win::message(TextKey::hotkey_hover_off_error, {{"code", std::to_wstring(*error)}})};
        }
        return {ShortcutState::off, tr(TextKey::hotkey_off_available)};
    }
    return status(action);
}
void Tray::settings() {
    auto snapshot = actions_.snapshot ? actions_.snapshot() : SettingsSnapshot{};
    snapshot.binding = [this](auto action) { return bindings_[action]; };
    snapshot.status = [this](auto action) { return status(action); };
    snapshot.configure = [this](auto action, auto candidate) { return configure(action, std::move(candidate)); };
    bool conflict = (bindings_[0].enabled && !registrations_[0]) || (bindings_[1].enabled && !registrations_[1]);
    settings_.open(std::move(snapshot), conflict);
}
void Tray::menu(POINT point) {
    using namespace tray_command;
    if (PopupMenuHost::input_active()) return;
    menu_mark(MenuStage::input);
    const RECT icon = notification_anchor(listener_, point);
    if (IsRectEmpty(&icon)) return;
    menu_mark(MenuStage::focus);
    const auto model = actions_.menu();
    const auto count = std::min<size_t>(preview_limit, model.items.size());
    std::vector<PopupItem> rows;
    rows.push_back({open, model.rope, fluent::Icon::rope});
    rows.back().detail = tr(TextKey::tray_current_rope);
    for (size_t i = 0; i < count; ++i) {
        rows.push_back({first_preview + int(i), menu_text(model.items[i].summary), fluent::Icon::document});
        rows.back().hint = std::to_wstring(i + 1) + L".";
    }
    if (!count) rows.push_back({empty, tr(TextKey::tray_empty), 0, false});
    rows.push_back({quick_note, tr(TextKey::tray_quick_note)});
    rows.back().hint = shortcut_hint(application::quick_note);
    if (model.hidden_card) rows.push_back({reveal_hidden, tr(TextKey::tray_reveal_hidden), fluent::Icon::document});
    rows.push_back({tray_command::settings, tr(TextKey::settings_global), fluent::Icon::settings});
    rows.push_back({tray_command::exit, tr(TextKey::tray_exit), fluent::Icon::exit});
    SetForegroundWindow(listener_);
    const int id = PopupMenuHost::track(listener_, {icon.left, icon.top}, L"ClipRope", tr(TextKey::tray_menu), std::move(rows),
                                        PopupFace::tray, {}, return_notification_focus, &icon);
    if (!IsWindow(listener_)) return;
    PostMessageW(listener_, WM_NULL, 0, 0);
    if (id == open) actions_.open_workspace();
    else if (id == reveal_hidden && model.hidden_card) actions_.reveal_hidden(*model.hidden_card);
    else if (id == quick_note) actions_.create_quick_note();
    else if (id == tray_command::settings) settings();
    else if (id == tray_command::exit) actions_.exit();
    else if (id == head && count) actions_.head(model.items.front().id, model.rope_id);
    else if (id >= first_preview && size_t(id - first_preview) < count)
        actions_.recall(model.items[id - first_preview].id, model.rope_id);
}
std::optional<LRESULT> Tray::message(UINT msg,WPARAM w,LPARAM l) {
    if (!listener_) return {};
    if (msg==WM_DESTROY) { stop(); return {}; }
    if (shell_restart_ && msg==shell_restart_) { retries_ = 0; recover_icon(true); return 0; }
    if (msg == WM_TIMER && w == icon_retry) { recover_icon(true); return 0; }
    if (icon_added_ && (msg==WM_DPICHANGED || msg==WM_DISPLAYCHANGE || msg==WM_SETTINGCHANGE)) {
        auto data=icon_data(listener_);
        data.uFlags=NIF_ICON;
        data.hIcon=notification_icon();
        if (!shell_(NIM_MODIFY,&data)) { retries_ = 0; recover_icon(true); }
    }
    try {
        if (msg==WM_COMMAND && w==tray_command::settings && !l) { settings(); return 0; }
        if (msg==WM_COMMAND && w==tray_command::quick_note && !l) { actions_.create_quick_note(); return 0; }
        if (msg == WM_HOTKEY) {
            for (auto action : {application::workspace_toggle, application::quick_note}) {
                if (!registrations_[action] || w != WPARAM(registrations_[action])) continue;
                const Hotkey event{LOWORD(l), HIWORD(l)};
                if (parse_hotkey(bindings_[action].chord) != event) return 0;
                if (action == application::quick_note) actions_.create_quick_note();
                else actions_.toggle();
                return 0;
            }
        }
        if (msg==callback && HIWORD(l)==icon_id) {
            switch (LOWORD(l)) {
            case NIN_SELECT:
            case NIN_KEYSELECT: actions_.toggle(); break;
            case WM_CONTEXTMENU: menu({GET_X_LPARAM(w),GET_Y_LPARAM(w)}); break;
            }
            return 0;
        }
    } catch (...) { notify(tr(TextKey::error_action)); return 0; }
    return {};
}
}
