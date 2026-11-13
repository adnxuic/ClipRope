#include "platform_win/settings.hpp"
#include "platform_win/app_icon.hpp"
#include "platform_win/fluent_list.hpp"
#include "platform_win/color_picker.hpp"

#include <algorithm>

#include <commctrl.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace cliprope::win {
using namespace fluent;
namespace {
constexpr wchar_t settings_class[] = L"ClipRope.Settings";
constexpr wchar_t viewport_class[] = L"ClipRope.SettingsViewport";
constexpr int chrome_height = 34;
constexpr int choice_width = 164, shortcut_width = 140, check_width = 122;
constexpr int segment_width = 76;
constexpr int nav_first = 1500;
constexpr int shortcut_first = 1800;
enum PreferenceId { light = 1900, dark, system, highlight, browse, restart, cancel_move, usage, defer_move };
constexpr UINT_PTR usage_timer = 1960;
const UiText themes[]{tr(TextKey::settings_theme_light), tr(TextKey::settings_theme_dark), tr(TextKey::settings_theme_system)};
enum RowControl { enable, record, check, controls_per_row = 4 }; // Preserve the native control IDs.
constexpr UINT chord_recorded = WM_APP + 0x450;

struct Page {
    UiText name;
    wchar_t glyph;
};
const Page pages[] = {
    {tr(TextKey::settings_general), Icon::settings}, {tr(TextKey::settings_cards), Icon::document},
    {tr(TextKey::settings_ropes), Icon::rope}, {tr(TextKey::settings_shortcuts), Icon::keyboard},
    {tr(TextKey::settings_appearance), Icon::appearance}, {tr(TextKey::settings_storage), Icon::lock},
    {tr(TextKey::settings_about), Icon::information}};
UiText check_label(ShortcutState state) {
    using enum ShortcutState;
    switch (state) {
    case registered: return tr(TextKey::hotkey_check_registered);
    case available: return tr(TextKey::hotkey_check_available);
    case off: return tr(TextKey::hotkey_check_off);
    case unverified: return tr(TextKey::hotkey_check_unverified);
    case limited: return tr(TextKey::hotkey_check_limited);
    case unavailable:
    case save_failed: return tr(TextKey::hotkey_check_unavailable);
    }
    return tr(TextKey::hotkey_check_action);
}
bool available(ShortcutState state) {
    return state == ShortcutState::registered || state == ShortcutState::available || state == ShortcutState::off;
}
} // namespace

std::wstring Settings::wide(std::string_view s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring result(n, L'\0');
    if (n)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), result.data(), n);
    return result;
}

thread_local Settings* Settings::recorder_{};

void Settings::shortcut_rows() {
    for (unsigned i = 0; i < application::shortcut_count; ++i) {
        const int base = shortcut_first + i * controls_per_row;
        Row row{};
        const UiText name = shortcut_labels[i];
        constexpr auto button = WS_TABSTOP | BS_NOTIFY;
        row.toggle = child(viewport_, L"BUTTON", message(TextKey::hotkey_enable, {{"action", name}}), button | BS_CHECKBOX, base + enable);
        if (i == 0) heading(row, tr(TextKey::settings_shortcuts_global));
        if (i == application::next_rope) heading(row, tr(TextKey::hotkey_hover_group));
        row.label = child(viewport_, L"STATIC", name, SS_NOPREFIX, 0);
        row.detail = child(viewport_, L"STATIC", L"", SS_NOPREFIX | SS_EDITCONTROL, 0);
        surface_.font(row.detail, Font::detail);
        row.value = child(viewport_, L"BUTTON", L"", button | BS_PUSHBUTTON, base + record);
        row.check = child(viewport_, L"BUTTON", L"", button, base + check);
        surface_.font(row.label);
        surface_.card_style(row.toggle, {CardPart::settings_switch});
        rows_.push_back(row);
    }
    sync_bindings();
}

void Settings::show_chord(application::ShortcutAction action) {
    auto h = rows_[action].value;
    auto chord = wide(shortcuts_[action].binding->chord);
    set_ui_text(h, message(TextKey::hotkey_chord, {{"action", shortcut_labels[action]}, {"chord", chord}}));
    surface_.card_style(h, {.part = CardPart::dialog_button, .label = chord});
}
void Settings::sync_bindings() {
    for (unsigned i = 0; i < shortcuts_.size(); ++i) {
        auto action = static_cast<application::ShortcutAction>(i);
        auto& view = shortcuts_[i];
        auto binding = data_.binding(action);
        auto status = data_.status(action);
        const bool changed = !view.binding || *view.binding != binding || view.status != status;
        if (!changed) continue;
        const bool chord_changed = !view.binding || view.binding->chord != binding.chord;
        if (!view.binding || view.binding->enabled != binding.enabled)
            SendMessageW(rows_[i].toggle, BM_SETCHECK, binding.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
        view.binding = std::move(binding);
        view.status = std::move(status);
        if (chord_changed) show_chord(action);
        show_check(action);
    }
}
void Settings::close_tip() {
    tooltip_.clear();
    tip_ = tip_source_ = nullptr;
    tip_message_.clear();
}
void Settings::show_tip(HWND source, UiText text, bool immediate) {
    close_tip();
    tip_source_ = source;
    tip_message_ = std::move(text);
    tooltip_.track(window_,source,tip_message_,immediate);
    tip_ = tooltip_.window();
}
void Settings::show_check(application::ShortcutAction action, const ShortcutResult* feedback) {
    auto& view = shortcuts_[action];
    auto h = rows_[action].check;
    const UINT_PTR timer = shortcut_first + action;
    KillTimer(window_, timer);
    view.deadline = 0;
    if (tip_source_ == h) close_tip();
    // A saved conflict remains discoverable on entry and after a failed retry.
    const auto* display = feedback;
    if (!display && view.status.state == ShortcutState::unavailable) display = &view.status;
    const UiText label = display ? check_label(display->state) : tr(TextKey::hotkey_check_action);
    UiText name = message(TextKey::hotkey_check_for, {{"state", label}, {"action", shortcut_labels[action]}});
    if (caption(h) != name) {
        set_ui_text(h, name);
        surface_.card_style(h, {.part = CardPart::dialog_button, .label = display ? label : tr(TextKey::hotkey_check_short)});
    }
    UiText description = display ? display->description : UiText{};
    if (description != view.description) {
        view.description = std::move(description);
        accessible_description(h, view.description);
    }
    const UiText inline_error = display && !available(display->state) ? display->description : UiText{};
    if (caption(rows_[action].detail) != inline_error) {
        set_ui_text(rows_[action].detail, inline_error);
        layout();
    }
    if (!feedback) return;
    announce_in_viewport(viewport_, shortcut_labels[action], feedback->description);
    if (available(feedback->state)) {
        view.deadline = GetTickCount64() + 3000;
        if (!SetTimer(window_, timer, 3000, nullptr)) {
            show_check(action);
            return;
        }
    }
    if (!available(feedback->state) || feedback->state == ShortcutState::available)
        show_tip(h, feedback->description, !available(feedback->state));
}
void Settings::clear_feedback() {
    close_tip();
    for (unsigned i = 0; i < shortcuts_.size(); ++i) KillTimer(window_, shortcut_first + i);
    shortcuts_ = {};
}
void Settings::configure_binding(application::ShortcutAction action, std::optional<application::ShortcutBinding> binding, HWND source) {
    const auto old = *shortcuts_[action].binding;
    auto result = std::make_shared<ShortcutResult>();
    submit_setting(rows_[action].detail, [configure = data_.configure, result, action, binding] { *result = configure(action, binding); },
        [this, action, binding, source, old, result](auto error) {
            if (error) *result = {ShortcutState::save_failed, tr(TextKey::error_setting_save)};
            sync_bindings();
            if (!binding) { show_check(action, result.get()); return; }
            if (old != *shortcuts_[action].binding) {
                // Disabled rows can also have a recent probe for either chord.
                for (unsigned i = 0; i < shortcuts_.size(); ++i) {
                    const auto& view = shortcuts_[i];
                    if (i != action && !view.binding->enabled &&
                        (view.binding->chord == old.chord || view.binding->chord == binding->chord))
                        show_check(static_cast<application::ShortcutAction>(i));
                }
            }
            if (result->state == ShortcutState::unavailable || result->state == ShortcutState::save_failed) {
                set_ui_text(rows_[action].detail, result->description);
                layout();
                accessible_description(source, result->description);
                show_tip(source, result->description, true);
                announce_in_viewport(viewport_, shortcut_labels[action], result->description);
            }
        });
}
void Settings::begin_recording(application::ShortcutAction action) {
    end_recording();
    close_tip();
    show_check(action);
    SetFocus(rows_[action].value);
    record_hook_ = SetWindowsHookExW(WH_KEYBOARD_LL, recording_hook, GetModuleHandleW(nullptr), 0);
    if (!record_hook_) {
        const UiText failure = tr(TextKey::hotkey_record_failed);
        accessible_description(rows_[action].value, failure);
        show_tip(rows_[action].value, failure, true);
        announce_in_viewport(viewport_, shortcut_labels[action], failure);
        return;
    }
    recorder_ = this;
    recording_ = action;
    ++recording_session_;
    set_ui_text(rows_[action].value, message(TextKey::hotkey_recording, {{"action", shortcut_labels[action]}}));
    surface_.card_style(rows_[action].value, {.part = CardPart::dialog_button, .label = tr(TextKey::hotkey_record_prompt), .selected = true});
    accessible_description(rows_[action].value, tr(TextKey::hotkey_record_description));
}
void Settings::end_recording() {
    if (!record_hook_) return;
    UnhookWindowsHookEx(record_hook_);
    record_hook_ = nullptr;
    recorder_ = nullptr;
    captured_ = {};
    const auto action = recording_;
    recording_ = application::shortcut_count;
    accessible_description(rows_[action].value, L"");
    show_chord(action);
}
LRESULT CALLBACK Settings::recording_hook(int code, WPARAM kind, LPARAM data) {
    auto* self = recorder_;
    if (code != HC_ACTION || !self || GetForegroundWindow() != self->window_ ||
        GetFocus() != self->rows_[self->recording_].value) return CallNextHookEx(nullptr, code, kind, data);
    const UINT key = reinterpret_cast<const KBDLLHOOKSTRUCT*>(data)->vkCode;
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU || key == VK_LWIN || key == VK_RWIN ||
        (key >= VK_LSHIFT && key <= VK_RMENU)) return CallNextHookEx(nullptr, code, kind, data);
    const bool down = kind == WM_KEYDOWN || kind == WM_SYSKEYDOWN;
    if (down && !self->captured_.key) {
        // The low-level callback runs before the main key updates. Modifiers are earlier packets.
        UINT mods = (GetAsyncKeyState(VK_CONTROL) < 0 ? MOD_CONTROL : 0) |
                    (GetAsyncKeyState(VK_SHIFT) < 0 ? MOD_SHIFT : 0) |
                    (GetAsyncKeyState(VK_MENU) < 0 ? MOD_ALT : 0) |
                    (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0 ? MOD_WIN : 0);
        self->captured_ = {mods, key};
    }
    if (!down && key == self->captured_.key)
        PostMessageW(self->window_, chord_recorded, self->recording_session_, 0);
    return 1;
}
bool Settings::shortcut_control(int id, int notification) {
    const int index = (id - shortcut_first) / controls_per_row;
    const int part = (id - shortcut_first) % controls_per_row;
    if (selected_ != 3 || id < shortcut_first || index >= int(rows_.size())) return false;
    const auto action = static_cast<application::ShortcutAction>(index);
    if (notification == BN_KILLFOCUS && recording_ == action && part == record) end_recording();
    if (notification == BN_KILLFOCUS && tip_source_ == GetDlgItem(viewport_, id)) close_tip();
    if (notification == BN_SETFOCUS) {
        const auto& row = rows_[index];
        if (row.top < offset_) scroll(row.top - 8);
        else if (row.top + row.height > offset_ + view_height_) scroll(row.top + row.height - view_height_ + 8);
    }
    if (notification != BN_CLICKED) return true;
    if (part == record) begin_recording(action);
    else if (part == check) {
        close_tip();
        configure_binding(action, std::nullopt, rows_[action].check);
    } else if (part == enable) {
        close_tip();
        accessible_description(rows_[action].toggle, L"");
        auto binding = data_.binding(action);
        binding.enabled = !binding.enabled;
        configure_binding(action, std::move(binding), rows_[action].toggle);
    }
    return true;
}

Settings::~Settings() { close(); }
void Settings::close() {
    if (window_)
        DestroyWindow(window_);
}
void Settings::submit_setting(HWND detail, std::function<void()> write, std::function<void(std::exception_ptr)> reply) {
    const auto previous = ui_caption(detail);
    const bool queued = saving_.submit(data_.defer_history, std::move(write),
        [this, detail, previous, reply = std::move(reply)](std::exception_ptr error) {
            EnableWindow(viewport_, TRUE);
            set_ui_text(detail, previous);
            reply(error);
            if (auto next = std::exchange(next_page_, {}); next && window_) page(*next);
        }, false);
    if (queued && window_) {
        EnableWindow(viewport_, FALSE);
        set_ui_text(detail, tr(TextKey::common_saving));
        layout();
        announce_in_viewport(viewport_, tr(TextKey::settings_short_title), tr(TextKey::common_saving));
    }
}
bool Settings::dialog_message(MSG& msg) {
    if (!window_ || (msg.hwnd != window_ && !IsChild(window_, msg.hwnd)))
        return false;
    if (msg.message == WM_KEYDOWN) {
        if (msg.wParam == VK_ESCAPE && tip_ && IsWindowVisible(tip_)) {
            close_tip();
            return true;
        }
        const int id = GetDlgCtrlID(msg.hwnd);
        if ((id == card_font || id == rope_capacity) && (msg.wParam == VK_RETURN || msg.wParam == VK_ESCAPE)) {
            if (msg.wParam == VK_RETURN) card_control(id, EN_KILLFOCUS);
            else sync_card_control(id);
            return true;
        }
        if ((id == card_font || id == rope_capacity || card_choice(id)) &&
            (msg.wParam == VK_HOME || msg.wParam == VK_END || msg.wParam == VK_PRIOR || msg.wParam == VK_NEXT))
            return false;
        if (selected_ == 3 && id >= shortcut_first && id < shortcut_first + int(application::shortcut_count) * controls_per_row &&
            (id - shortcut_first) % controls_per_row == record && msg.wParam == VK_RETURN) {
            if (!(msg.lParam & (LPARAM(1) << 30))) begin_recording(static_cast<application::ShortcutAction>((id - shortcut_first) / controls_per_row));
            return true;
        }
        const int key = int(msg.wParam);
        if (selected_ == 4 && id >= light && id <= system && (key == VK_LEFT || key == VK_RIGHT)) {
            const int next = light + (id-light+(key==VK_LEFT ? 2 : 1))%3;
            SetFocus(GetDlgItem(viewport_,next));
            preference_control(next,BN_CLICKED);
            return true;
        }
        if (key == VK_NEXT || key == VK_PRIOR || key == VK_HOME || key == VK_END) {
            scrollbar_.key(key);
            return true;
        }
    }
    return IsDialogMessageW(window_, &msg) != FALSE;
}
void Settings::open(SettingsSnapshot data, bool conflict) {
    if (saving_.pending() && window_) { SetForegroundWindow(window_); return; }
    data_ = std::move(data);
    if (conflict)
        selected_ = 3;
    if (!window_) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc;
        wc.lpszClassName = settings_class;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return;
        const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                    ex = WS_EX_APPWINDOW | WS_EX_CONTROLPARENT;
        POINT point{};
        GetCursorPos(&point);
        MONITORINFO monitor{sizeof(monitor)};
        const auto display = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
        GetMonitorInfoW(display, &monitor);
        UINT physical = GetDpiForSystem(), dy = physical;
        GetDpiForMonitor(display, MDT_EFFECTIVE_DPI, &physical, &dy);
        const auto dpi = design_dpi(physical);
        RECT rect{0, 0, MulDiv(width_, dpi, 96), MulDiv(height_, dpi, 96)};
        const auto& work = monitor.rcWork;
        const int w = std::min(rect.right - rect.left, work.right - work.left);
        const int h = std::min(rect.bottom - rect.top, work.bottom - work.top);
        if (!create_ui_window(
                ex, settings_class, tr(TextKey::settings_window_title), style, work.left + (work.right - work.left - w) / 2,
                work.top + (work.bottom - work.top - h) / 2, w, h, nullptr, nullptr, wc.hInstance, this))
            return;
    }
    page(selected_);
    ShowWindow(window_, IsIconic(window_) ? SW_RESTORE : SW_SHOW);
    SetForegroundWindow(window_);
    SetFocus(GetDlgItem(window_, nav_first + selected_));
}
void Settings::page(int index) {
    if (saving_.pending()) { next_page_ = index; return; }
    const auto focus = GetFocus();
    if (focus && GetParent(focus) == viewport_) {
        const int id = GetDlgCtrlID(focus);
        if (id == card_font || id == rope_capacity) card_control(id, EN_KILLFOCUS);
    }
    if (saving_.pending()) { next_page_ = index; return; }
    end_recording();
    KillTimer(window_, usage_timer);
    clear_feedback();
    selected_ = -1; // Ignore focus notifications while retiring the previous page.
    offset_ = 0;
    for (auto& r : rows_)
        for (auto h : {r.label, r.detail, r.value, r.toggle, r.check, r.defer, r.heading})
            if (h) DestroyWindow(h);
    rows_.clear();
    selected_ = index;
    set_ui_text(title_, pages[index].name);
    switch (index) {
    case 0:
        general_rows();
        break;
    case 1: case 2:
        card_rows();
        break;
    case 3:
        shortcut_rows();
        break;
    case 4: case 5:
        preference_rows();
        break;
    case 6: {
        auto readout = [&](UiText label, UiText value) {
            Row row{};
            row.label = child(viewport_, L"STATIC", label, SS_NOPREFIX, 0);
            row.value = child(viewport_, L"STATIC", value, SS_NOPREFIX | SS_RIGHT, 0);
            surface_.font(row.label);
            surface_.font(row.value);
            accessible_name(row.value, message(TextKey::settings_read_only, {{"label", label}, {"value", value}}));
            rows_.push_back(row);
        };
        readout(tr(TextKey::settings_version), L"" CLIPROPE_VERSION);
        readout(tr(TextKey::settings_about_storage_engine), L"SQLite");
        break;
    }
    }
    layout();
}
void Settings::heading(Row& row, UiText text) {
    row.heading = child(viewport_, L"STATIC", text, SS_NOPREFIX, 0);
    surface_.font(row.heading, Font::section);
}
void Settings::preference_rows() {
    auto row = [&](UiText name) -> Row& {
        auto label = child(viewport_, L"STATIC", name, SS_NOPREFIX, 0);
        auto detail = child(viewport_, L"STATIC", L"", SS_NOPREFIX | SS_EDITCONTROL, 0);
        surface_.font(label);
        surface_.font(detail, Font::detail);
        return rows_.emplace_back(Row{label, detail});
    };
    auto button = [&](UiText name, int id, DWORD kind = BS_PUSHBUTTON) {
        return child(viewport_, L"BUTTON", name, WS_TABSTOP | BS_NOTIFY | kind, id);
    };
    if (selected_ == 4) {
        auto& theme = row(tr(TextKey::settings_theme));
        theme.value = button(themes[0], light, WS_GROUP | BS_RADIOBUTTON);
        theme.toggle = button(themes[1], dark, BS_RADIOBUTTON);
        theme.check = button(themes[2], system, BS_RADIOBUTTON);
        auto& color = row(tr(TextKey::settings_match_color));
        color.value = button(L"", highlight, WS_GROUP);
    } else {
        auto& path = row(tr(TextKey::settings_data_path));
        heading(path, tr(TextKey::settings_storage_group));
        path.value = button(tr(TextKey::settings_data_path_browse), browse);
        path.toggle = button(tr(TextKey::settings_data_path_restart), restart);
        path.check = button(tr(TextKey::settings_data_path_cancel), cancel_move);
        path.defer = button(tr(TextKey::settings_data_path_later), defer_move);
        for (auto h : {path.value, path.toggle, path.check, path.defer})
            surface_.card_style(h, {.part = CardPart::dialog_button, .label = ui_caption(h)});
        ShowWindow(path.defer, SW_HIDE);
        EnableWindow(path.value, bool(data_.schedule));
        auto& size = row(tr(TextKey::settings_disk_usage));
        size.value = child(viewport_, L"STATIC", tr(TextKey::settings_disk_usage_measuring), SS_NOPREFIX | SS_RIGHT, usage);
        surface_.font(size.value, Font::detail);
        auto& privacy = row(tr(TextKey::settings_exclusions));
        heading(privacy, tr(TextKey::settings_privacy));
        set_ui_text(privacy.detail, tr(TextKey::settings_exclusions_description));
        const auto edit = tr(TextKey::settings_exclusions_edit);
        privacy.value = button(edit, exclusions);
        surface_.card_style(privacy.value, {.part = CardPart::dialog_button, .label = edit});
        accessible_description(privacy.value, tr(TextKey::settings_exclusions_description));
        EnableWindow(privacy.value, data_.read_exclusions && data_.save_exclusions);
        next_measure_ = 0;
        SetTimer(window_, usage_timer, 250, nullptr);
        measure_usage();
    }
    show_preferences();
}
void Settings::show_preferences() {
    if (selected_ == 4) {
        constexpr CardPart parts[]{CardPart::segment_left, CardPart::segment_middle, CardPart::segment_right};
        for (int i = 0; i < 3; ++i) {
            const auto h = GetDlgItem(viewport_, light + i);
            const bool selected = int(data_.appearance.theme) == i;
            SendMessageW(h, BM_SETCHECK, selected ? BST_CHECKED : BST_UNCHECKED, 0);
            surface_.card_style(h, {.part = parts[i], .label = themes[i], .selected = selected});
        }
        wchar_t value[12]{};
        swprintf_s(value, L"#%06X", data_.appearance.match_rgb());
        const auto h = rows_[1].value;
        set_ui_text(h, message(TextKey::settings_color_accessible, {{"color", value}}));
        color_picker_button(surface_, h, data_.appearance.match_color, domain::match_default, ColorButtonStyle::settings);
    } else {
        UiText value = message(TextKey::settings_path_current, {{"path", data_.profile.wstring()}});
        if (!data_.pending.empty())
            value += message(TextKey::settings_path_pending, {{"path", data_.pending.wstring()}});
        if (!data_.schedule) value += tr(TextKey::settings_path_isolated);
        if (!data_.location_notice.empty()) value += L"\r\n" + data_.location_notice;
        set_ui_text(rows_[0].detail, value);
        for (auto h : {rows_[0].toggle, rows_[0].check})
            ShowWindow(h, data_.pending.empty() ? SW_HIDE : SW_SHOWNA);
    }
}
void Settings::measure_usage() {
    if (selected_ != 5 || !data_.measure) return;
    try {
        if (measurement_.valid()) {
            if (measurement_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            auto bytes = measurement_.get();
            double value = double(bytes);
            constexpr const wchar_t* units[]{L"B", L"KiB", L"MiB", L"GiB"};
            int unit = 0;
            while (value >= 1024 && unit < 3) {
                value /= 1024;
                ++unit;
            }
            wchar_t label[64]{};
            swprintf_s(label, unit ? L"%.2f %s" : L"%.0f %s", value, units[unit]);
            if (caption(rows_[1].value) != label) {
                set_ui_text(rows_[1].value, label);
                set_ui_text(rows_[1].detail, L"");
                accessible_description(rows_[1].value, tr(TextKey::settings_disk_usage_accessible));
                layout();
            }
            next_measure_ = GetTickCount64() + 4000;
        } else if (GetTickCount64() >= next_measure_) {
            measurement_ = std::async(std::launch::async, data_.measure);
        }
    } catch (const std::exception&) {
        const auto label = tr(TextKey::hotkey_check_unavailable);
        set_ui_text(rows_[1].value, label);
        set_ui_text(rows_[1].detail, tr(TextKey::settings_disk_usage_unavailable));
        accessible_description(rows_[1].value, tr(TextKey::settings_disk_usage_unavailable));
        next_measure_ = GetTickCount64() + 4000;
        layout();
    }
}
bool Settings::preference_control(int id, int notification) {
    if ((selected_ != 4 && selected_ != 5) || ((id < light || id > defer_move) && id != exclusions) || id == usage) return false;
    auto source = GetDlgItem(viewport_, id);
    if (!source || !IsWindowEnabled(source)) return true;
    if (notification == BN_SETFOCUS) {
        auto& r = rows_[id == exclusions ? 2 : id == highlight ? 1 : 0];
        if (r.top < offset_)
            scroll(r.top - 8);
        else if (r.top + r.height > offset_ + view_height_)
            scroll(r.top + r.height - view_height_ + 8);
    }
    if (notification != BN_CLICKED) return true;
    try {
        if (id == exclusions) {
            edit_exclusions();
            if (!window_ || selected_ != 5) return true;
            set_ui_text(rows_[2].detail, tr(TextKey::settings_exclusions_description));
            accessible_description(source, tr(TextKey::settings_exclusions_description));
            layout();
            return true;
        }
        if (selected_ == 4) {
            auto value = data_.appearance;
            if (id == highlight) {
                ColorPickerOptions options;
                options.defer_history = data_.defer_history;
                options.value = value.match_color;
                options.default_color = domain::match_default;
                options.callbacks.preview = [this](auto color) { if(data_.preview_match) data_.preview_match(color); };
                options.callbacks.cancel = [this] { if(data_.preview_match) data_.preview_match({}); };
                options.callbacks.commit = [this, original=value.match_color](auto color) {
                    data_.save_match(original,color);
                    data_.appearance.match_color=color;
                    if(data_.preview_match) data_.preview_match({});
                    return presentation::ColorCommit::saved;
                };
                ColorPickerPopup::track(window_, source, std::move(options));
                show_preferences();
                return true;
            } else
                value.theme = static_cast<application::Theme>(id - light);
            if (value != data_.appearance) {
                show_preferences();
                submit_setting(rows_[0].detail, [save = data_.save_appearance, value] { save(value); }, [this, value, id](auto error) {
                    if (error) { preference_error(id); return; }
                    data_.appearance = value;
                    set_ui_text(rows_[0].detail, L"");
                    set_ui_text(rows_[1].detail, L"");
                    layout();
                    show_preferences();
                });
            } else show_preferences();
        } else if (id == defer_move) {
            ShowWindow(source, SW_HIDE);
            layout();
            SetFocus(rows_[0].value);
        } else if (id == restart) {
            if (!data_.pending.empty()) submit_setting(rows_[0].detail, data_.restart, [this, id](auto error) { if (error) preference_error(id); });
        } else {
            std::filesystem::path next;
            if (id == browse) {
                Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
                FILEOPENDIALOGOPTIONS flags{};
                if (FAILED(
                        CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))) ||
                    FAILED(dialog->GetOptions(&flags)) ||
                    FAILED(dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                                              FOS_DONTADDTORECENT)))
                    throw std::runtime_error("Cannot open the folder picker.");
                const auto parent = (data_.pending.empty() ? data_.profile : data_.pending).parent_path();
                Microsoft::WRL::ComPtr<IShellItem> initial;
                if (SUCCEEDED(SHCreateItemFromParsingName(parent.c_str(), nullptr, IID_PPV_ARGS(&initial))))
                    dialog->SetFolder(initial.Get());
                dialog->SetTitle(ui_text(TextKey::settings_data_path_choose));
                auto hr = dialog->Show(window_);
                if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return true;
                Microsoft::WRL::ComPtr<IShellItem> item;
                PWSTR name{};
                if (FAILED(hr) || FAILED(dialog->GetResult(&item)) ||
                    FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)))
                    throw std::runtime_error("Cannot read the selected folder.");
                std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(name, CoTaskMemFree);
                next = name;
            }
            submit_setting(rows_[0].detail, [schedule = data_.schedule, next] { schedule(next); }, [this, next, id](auto error) {
                if (error) { preference_error(id); return; }
                data_.pending = next;
                data_.location_notice.clear();
                show_preferences();
                ShowWindow(rows_[0].defer, next.empty() ? SW_HIDE : SW_SHOWNA);
                layout();
                if (!next.empty()) SetFocus(rows_[0].defer);
            });
        }
    } catch (const std::exception&) {
        preference_error(id);
    }
    return true;
}
void Settings::preference_error(int id) {
    const auto source = GetDlgItem(viewport_, id);
    const auto message = selected_ == 4 ? tr(TextKey::error_appearance_save) : tr(TextKey::error_setting_save);
    show_preferences();
    auto detail = rows_[id == exclusions ? 2 : id == highlight ? 1 : 0].detail;
    if (selected_ == 5 && id != exclusions)
        set_ui_text(detail, (ui_caption(detail) + L"\r\n" + message));
    else
        set_ui_text(detail, message);
    layout();
    accessible_description(source, message);
    announce_in_viewport(viewport_, tr(TextKey::settings_short_title), message);
}
void Settings::retranslate() {
    const auto tip_source = tip_source_;
    const auto tip_message = tip_message_;
    const bool visible_tip = tip_ && IsWindowVisible(tip_);
    refresh_ui_bindings(window_);
    if (selected_ == 0) sync_language_choice(true);
    if (selected_ == 1 && !rows_.empty()) {
        for (int id : card_order) {
            if (!card_choice(id)) continue;
            fill_card_choice(id);
            sync_card_control(id);
        }
    }
    layout();
    if (visible_tip && IsWindow(tip_source)) show_tip(tip_source, tip_message, true);
}
void Settings::layout() {
    if (!title_) return;
#ifdef CLIPROPE_DESKTOP_PROBES
    ++layout_count_;
#endif
    close_tip();
    RECT client{};
    GetClientRect(window_, &client);
    int w = dip(window_, client.right), h = dip(window_, client.bottom);
    sidebar_ = w < 760 ? 64 : 180;
    for (int i = 0; i < int(std::size(pages)); ++i) {
        auto b = GetDlgItem(window_, nav_first + i);
        surface_.card_style(b, {CardPart::settings_nav, pages[i].glyph, sidebar_ == 64 ? L"" : pages[i].name, {}, {}, 0, selected_ == i});
        SendMessageW(b, BM_SETCHECK, selected_ == i ? BST_CHECKED : BST_UNCHECKED, 0);
        place(b, 10, i == 6 ? h - 47 : 76 + i * 38, sidebar_ - 20, 35);
    }
    body_width_ = std::min(660, w - sidebar_ - 2 * margin_);
    HDC dc = GetDC(window_);
    const auto original = GetCurrentObject(dc, OBJ_FONT);
    auto measured = [&](HWND control, int width) {
        if (!control) return 0;
        const auto value = caption(control);
        if (value.empty()) return 0;
        SelectObject(dc, HFONT(SendMessageW(control, WM_GETFONT, 0, 0)));
        RECT rect{0, 0, px(window_, width), 0};
        DrawTextW(dc, value.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        return dip(window_, rect.bottom) + 2;
    };
    int top = 48;
    for (auto& r : rows_) {
        const int id = GetDlgCtrlID(r.value);
        if (r.heading) top += (&r == &rows_.front() ? 0 : 20) + 26;
        r.top = top;
        if (selected_ == 3) r.control_width = 42 + 8 + shortcut_width + 8 + check_width;
        else if (id == light) r.control_width = 3 * segment_width;
        else if (id == language) r.control_width = 196;
        else if (card_choice(id)) r.control_width = choice_width;
        else if (id == card_font || id == rope_capacity) r.control_width = 76;
        else if (id == browse || id == exclusions || id == reset || id == highlight || selected_ == 6 || id == usage)
            r.control_width = 122;
        else r.control_width = 42;
        r.text_width = body_width_ - r.control_width - 16;
        r.label_height = measured(r.label, r.text_width);
        r.detail_height = measured(r.detail, id == reset ? body_width_ : r.text_width);
        r.height = std::max(44, 12 + r.label_height + (r.detail_height ? 2 + r.detail_height : 0));
        if (id == reset) r.height = 44 + r.detail_height;
        if (id == browse && !data_.pending.empty()) r.height += 38;
        top += r.height;
    }
    SelectObject(dc, original);
    ReleaseDC(window_, dc);
    view_height_ = std::max(1, h - chrome_height);
    max_offset_ = std::max(0, top + 20 - view_height_);
    offset_ = std::clamp(offset_, 0, max_offset_);
    place(viewport_, sidebar_, chrome_height, w - sidebar_ - 16, view_height_);
    place(scrollbar_.window(), w - 16, chrome_height + 4, 16, view_height_ - 12);
    position_rows();
    InvalidateRect(window_, nullptr, FALSE);
}
void Settings::position_rows() {
    const int left = margin_, right = left + body_width_;
    ChildLayout batch;
    batch.place(title_, left, -offset_, body_width_, 36);
    for (auto& r : rows_) {
        const int id = GetDlgCtrlID(r.value);
        const int y = r.top - offset_;
        if (r.heading) batch.place(r.heading, left, y - 26, body_width_, 24);
        if (id == reset) {
            batch.place(r.value, left, y + 4, r.control_width, 32);
            batch.place(r.detail, left, y + 40, body_width_, r.detail_height, r.detail_height ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
            continue;
        }
        const bool pending = id == browse && !data_.pending.empty();
        const int main_height = r.height - (pending ? 38 : 0);
        const int detail_gap = r.detail_height ? 2 : 0;
        const int text_y = y + (main_height - r.label_height - detail_gap - r.detail_height) / 2;
        batch.place(r.label, left, text_y, r.text_width, r.label_height);
        batch.place(r.detail, left, text_y + r.label_height + detail_gap, r.text_width, r.detail_height,
                    r.detail_height ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
        const int x = right - r.control_width;
        const int cy = y + (main_height - 32) / 2;
        if (selected_ == 3) {
            batch.place(r.toggle, x, cy, 42, 32);
            batch.place(r.value, x + 50, cy, shortcut_width, 32);
            batch.place(r.check, right - check_width, cy, check_width, 32);
        } else if (id == light) {
            batch.place(r.value, x, cy, segment_width, 32);
            batch.place(r.toggle, x + segment_width, cy, segment_width, 32);
            batch.place(r.check, x + 2 * segment_width, cy, segment_width, 32);
        } else {
            const bool read_only = id == usage || selected_ == 6;
            const int height = read_only ? 21 : id == highlight ? 36 : 32;
            const int full_height = id == language || card_choice(id) ? choice_height(r.value, height) : height;
            batch.place(r.value, x, y + (main_height - height) / 2, r.control_width, full_height);
            if (pending) {
                int action_x = left;
                for (HWND action : {r.toggle, r.defer, r.check}) {
                    if (!(GetWindowLongPtrW(action, GWL_STYLE) & WS_VISIBLE)) continue;
                    batch.place(action, action_x, y + main_height, 106, 30);
                    action_x += 112;
                }
            }
        }
    }
    batch.apply(viewport_, true);
    scrollbar_.update(offset_, max_offset_, view_height_, 16);
}
void Settings::scroll(int offset) {
    offset = std::clamp(offset, 0, max_offset_);
    if (offset != offset_) {
        close_tip();
        offset_ = offset;
        position_rows();
    }
}
void Settings::paint(HDC dc, bool body) {
    RECT rect{};
    GetClientRect(body ? viewport_ : window_, &rect);
    auto& p = surface_.colors;
    auto box = [&](int x, int y, int w, int h) {
        return RECT{px(window_, x), px(window_, y), px(window_, x + w), px(window_, y + h)};
    };
    const int h = dip(window_, rect.bottom);
    solid(dc, rect, p.window);
    if (!body) {
        solid(dc, box(0, 0, sidebar_, h), p.subtle);
        solid(dc, box(sidebar_ - 1, 0, 1, h), p.line);
        auto logo = box(sidebar_ == 64 ? 20 : 16, 19, 24, 24);
        surface_.draw_card(window_, dc, logo, {CardPart::brand_icon});
        if (sidebar_ > 64)
            surface_.draw_text(dc, box(48, 16, 146, 30), L"ClipRope", Font::section, p.text,
                               DT_SINGLELINE | DT_VCENTER);
        return;
    }
    for (const auto& row : rows_) {
        if (&row == &rows_.front()) continue;
        const int y = row.top - offset_;
        if (!row.heading) solid(dc, box(margin_, y, body_width_, 1), p.line);
        else if (GetDlgCtrlID(row.value) == reset) solid(dc, box(margin_, y - 39, body_width_, 1), p.line);
    }
}
LRESULT CALLBACK Settings::viewport_proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    const auto root = GetParent(h);
    auto* self = reinterpret_cast<Settings*>(GetWindowLongPtrW(root, GWLP_USERDATA));
    switch (msg) {
    case WM_COMMAND:
    case WM_NOTIFY:
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_MOUSEWHEEL:
        return SendMessageW(root, msg, w, l);
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        if (self) self->paint(dc, true);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, w, l);
}
LRESULT CALLBACK Settings::proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<Settings*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<Settings*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->window_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(self));
    }
    if (!self)
        return DefWindowProcW(h, msg, w, l);
    switch (msg) {
    case WM_CREATE: {
        set_application_icons(h);
        if (!self->surface_.dialog_chrome(h)) return -1;
        SetWindowTheme(h, self->surface_.colors.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        for (int i = 0; i < int(std::size(pages)); ++i)
            child(h, L"BUTTON", pages[i].name, WS_TABSTOP | BS_RADIOBUTTON, nav_first + i);
        WNDCLASSW view{};
        view.hInstance = GetModuleHandleW(nullptr);
        view.lpszClassName = viewport_class;
        view.lpfnWndProc = viewport_proc;
        view.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!RegisterClassW(&view) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return -1;
        auto viewport = create_ui_window(WS_EX_CONTROLPARENT, viewport_class, tr(TextKey::settings_content),
            WS_VISIBLE | WS_CHILD | WS_CLIPCHILDREN, 0, 0, 1, 1, h, nullptr, view.hInstance, nullptr);
        self->viewport_ = viewport;
        if (!viewport || !self->scrollbar_.create(h, viewport, self->surface_.colors, false,
                [self](int next) { self->scroll(next); })) return -1;
        self->title_ = child(viewport, L"STATIC", L"", SS_NOPREFIX, 0);
        self->surface_.font(self->title_, Font::title);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(w) >= nav_first && LOWORD(w) < nav_first + std::size(pages) && HIWORD(w) == BN_CLICKED) {
            self->page(LOWORD(w) - nav_first);
            return 0;
        }
        if (self->saving_.pending() && LOWORD(w) != IDCANCEL) return 0;
        if (self->general_control(LOWORD(w), HIWORD(w))) return 0;
        if (self->card_control(LOWORD(w), HIWORD(w))) return 0;
        if (self->preference_control(LOWORD(w),HIWORD(w))) return 0;
        if (self->shortcut_control(LOWORD(w), HIWORD(w))) return 0;
        if (LOWORD(w) == IDCANCEL) {
            DestroyWindow(h);
            return 0;
        }
        break;
    case chord_recorded: {
        if (!self->record_hook_ || w != self->recording_session_) return 0;
        const auto chord = self->captured_;
        const auto action = self->recording_;
        self->end_recording();
        if (chord.key == VK_ESCAPE) return 0;
        auto binding = self->data_.binding(action);
        binding.chord = hotkey_name(chord);
        self->configure_binding(action, std::move(binding), self->rows_[action].value);
        return 0;
    }
#ifdef CLIPROPE_DESKTOP_PROBES
    case WM_APP + 0x451: return self->layout_count_;
#endif
    case WM_TIMER:
        if (w == usage_timer) { if (!IsIconic(h)) self->measure_usage(); return 0; }
        if (w >= shortcut_first && w < shortcut_first + application::shortcut_count && self->selected_ == 3) {
            auto action = static_cast<application::ShortcutAction>(w - shortcut_first);
            const auto due = self->shortcuts_[action].deadline;
            if (due) {
                const auto now = GetTickCount64();
                if (now >= due) self->show_check(action);
                else SetTimer(h, w, UINT(due - now), nullptr);
            }
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            self->close_tip();
            self->end_recording();
        }
        break;
    case WM_DESTROY:
        self->end_recording();
        self->clear_feedback();
        break;
    case language_changed:
        self->retranslate();
        return 0;
    case WM_GETMINMAXINFO: {
        auto& size = reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize;
        size = {px(h, 680), px(h, 480)};
        return 0;
    }
    case WM_MOVE:
        self->close_tip();
        break;
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) {
            self->width_ = dip(h, LOWORD(l));
            self->height_ = dip(h, HIWORD(l));
            self->layout();
        }
        return 0;
    case WM_MOUSEWHEEL:
        self->scrollbar_.wheel(w);
        return 0;
    case WM_DPICHANGED: {
        set_application_icons(h, HIWORD(w));
        auto& r = *reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        [[fallthrough]];
    }
    case WM_SETTINGCHANGE: {
        const auto next = system_palette();
        if (next.dark != self->surface_.colors.dark)
            SetWindowTheme(h, next.dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        [[fallthrough]];
    }
    case WM_THEMECHANGED:
        self->surface_.refresh();
        self->layout();
        return 0;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        auto& palette = self->surface_.colors;
        SetTextColor(HDC(w), palette.text);
        SetBkColor(HDC(w), palette.panel);
        SetDCBrushColor(HDC(w), palette.panel);
        return LRESULT(GetStockObject(DC_BRUSH));
    }
    case WM_CTLCOLORSTATIC: {
        auto ctrl = HWND(l);
        auto dc = HDC(w);
        auto& theme = self->surface_;
        for (auto& r : self->rows_)
            if (ctrl == r.detail || ctrl == r.value) {
                auto brush = theme.control_color(dc, ctrl);
                SetTextColor(dc, theme.colors.muted);
                return brush;
            }
        return theme.control_color(dc, ctrl);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        self->paint(dc, false);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        self->saving_.close();
        self->next_page_.reset();
        self->rows_.clear();
        self->title_ = nullptr;
        self->window_ = nullptr;
        self->surface_.release();
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}
} // namespace cliprope::win
