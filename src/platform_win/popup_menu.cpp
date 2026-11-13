#include "platform_win/menu_timing.hpp"
#include "platform_win/menu_focus.hpp"
#include <dwmapi.h>
#include "platform_win/popup_menu.hpp"
#include "platform_win/color_picker.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <commctrl.h>
#include <cwchar>
#include <imm.h>
#include <shellscalingapi.h>
#include <windowsx.h>

namespace cliprope::win {
using namespace fluent;
namespace {
std::atomic_bool active{};
thread_local HWND pending_file_popup{}, active_window{};
constexpr UINT refresh_file = WM_APP + 249;
std::wstring hex(COLORREF color) {
    wchar_t text[8]{};
    swprintf_s(text, L"#%02X%02X%02X", GetRValue(color), GetGValue(color), GetBValue(color));
    return text;
}
COLORREF displayed_color(const PopupItem& item, size_t index) {
    if (index == 0 && std::none_of(item.palette.begin(), item.palette.end(),
                                   [&](auto c) { return _wcsicmp(item.value.c_str(), hex(c).c_str()) == 0; })) {
        const auto rgb = std::wcstoul(item.value.c_str() + 1, nullptr, 16);
        return RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255);
    }
    return item.palette[index];
}
} // namespace
int PopupMenuHost::row_height(const PopupItem& item) const {
    if (card())
        return item.kind == PopupKind::color     ? 36
               : item.kind == PopupKind::command ? (item.detail.empty() ? 24 : 36)
                                                 : 28;
    return item.kind == PopupKind::text ? 42 : 40;
}
bool PopupMenuHost::activates(HWND window) {
    return window && (window == active_window || ColorPickerPopup::owns(window));
}
bool PopupMenuHost::input_active() {
    return active.load() || ColorPickerPopup::active();
}
int PopupMenuHost::preferred_height() const {
    if (tray())
        return tray_height();
    if (rope())
        return rope_body_height() + error_height();
    int h = card() ? menu_header + menu_bottom + error_height() : 76 + 12 + (message_.empty() ? 0 : 58);
    for (auto& item : items_)
        h += row_height(item) + (item.divider ? (card() ? menu_divider : 9) : 0);
    return h;
}
int PopupMenuHost::error_height() const {
    if (message_.empty()) return 0;
    RECT client{};
    GetClientRect(window_, &client);
    RECT bounds{0, 0, std::max<LONG>(1, client.right - px(window_, 24)), 0};
    HDC dc = GetDC(window_);
    auto old = SelectObject(dc, theme_.get(Font::menu));
    DrawTextW(dc, message_.c_str(), -1, &bounds, DT_CALCRECT | DT_WORDBREAK | DT_EDITCONTROL | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(window_, dc);
    return dip(window_, bounds.bottom) + 8;
}
RECT PopupMenuHost::placement(HWND owner, POINT at, const RECT* exclude) const {
    MONITORINFO monitor{sizeof(monitor)};
    const auto display = exclude ? MonitorFromRect(exclude, MONITOR_DEFAULTTONEAREST)
                                 : MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(display, &monitor))
        return {};
    UINT dpi = GetDpiForWindow(owner), dy = dpi;
    GetDpiForMonitor(display, MDT_EFFECTIVE_DPI, &dpi, &dy);
    dpi = design_dpi(dpi);
    auto& work = monitor.rcWork;
    int w = std::min<int>(work.right - work.left, MulDiv(width_, dpi, 96));
    int h = std::min<int>(work.bottom - work.top, MulDiv(preferred_height(), dpi, 96));
    if (exclude) {
        // Align the visible face, allowing the transparent shadow to extend past the work area.
        const int gap = MulDiv(2, dpi, 96);
        RECT avoid = *exclude;
        InflateRect(&avoid, 0, gap);
        POINT anchor{std::clamp(at.x, work.left, work.right - 1),
                     std::clamp(std::min(at.y, work.bottom) - gap, work.top, work.bottom - 1)};
        const SIZE size{w, h};
        RECT face{};
        if (CalculatePopupWindowPosition(&anchor, &size, TPM_LEFTALIGN | TPM_BOTTOMALIGN | TPM_VERTICAL | TPM_WORKAREA,
                                         &avoid, &face))
            return face;
        const LONG x = std::clamp(anchor.x, work.left, work.right - w);
        const LONG y = std::clamp(anchor.y - h, work.top, work.bottom - h);
        return {x, y, x + w, y + h};
    }
    const int shadow = CardMenuFrame::shadow_margin(dpi);
    const int inset_x = std::min<int>(shadow, (work.right - work.left - w) / 2);
    const int inset_y = std::min<int>(shadow, (work.bottom - work.top - h) / 2);
    const int x = std::clamp<int>(at.x, work.left + inset_x, work.right - w - inset_x);
    const int y = std::clamp<int>(at.y, work.top + inset_y, work.bottom - h - inset_y);
    return {x, y, x + w, y + h};
}
int PopupMenuHost::track(HWND owner, POINT at, UiText title, UiText subtitle,
                         std::vector<PopupItem> items, PopupFace face, RefreshCommands refresh,
                         void (*return_focus)(HWND, int), const RECT* exclude, DeferHistoryCommand defer_history) {
    if (items.empty() || active.exchange(true))
        return 0;
    struct Guard {
        ~Guard() { pending_file_popup = active_window = nullptr; active = false; }
    } guard;
    const MenuFocus focus;
    menu_mark(MenuStage::model);
    PopupMenuHost host;
    host.defer_history_ = std::move(defer_history);
    host.face_ = face;
    host.owner_ = owner;
    if (exclude) host.exclusion_ = *exclude;
    host.anchor_ = at;
    host.refresh_ = std::move(refresh);
    host.title_ = std::move(title);
    host.subtitle_ = std::move(subtitle);
    host.items_ = std::move(items);
    const bool properties = std::any_of(host.items_.begin(), host.items_.end(),
                                        [](const auto& item) { return item.kind != PopupKind::command; });
    host.width_ = host.tray() ? 224 : host.rope() ? 320 : host.card() ? (properties ? 288 : 248) : properties ? 428 : 364;
    host.base_width_ = host.width_;
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.lpszClassName = host.tray() ? L"ClipRope.TrayMenu" : host.rope() ? L"ClipRope.RopeMenu"
                       : host.card() ? L"ClipRope.CardMenu" : L"ClipRope.PopupMenu";
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return 0;
    const RECT bounds = host.placement(owner, at, exclude);
    if (IsRectEmpty(&bounds)) return 0;
    const HWND parent = host.frame_.open(owner) ? host.frame_.window() : owner;
    const auto caption = face != PopupFace::standard ? host.title_ + L" — " + host.subtitle_ : host.title_;
    HWND popup =
        create_ui_window(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, wc.lpszClassName, caption,
                        WS_POPUP | WS_CLIPCHILDREN, bounds.left, bounds.top, bounds.right - bounds.left,
                        bounds.bottom - bounds.top, parent, nullptr, wc.hInstance, &host);
    if (!popup)
        return 0;
    host.size_for_labels();
    menu_mark(MenuStage::controls);
    if (host.frame_.window())
        host.frame_.fit(popup, host.theme_.colors, design_dpi(GetDpiForWindow(popup)));
    menu_mark(MenuStage::frame);
    active_window = popup;
    ShowWindow(popup, SW_SHOW);
    SetForegroundWindow(popup);
    SetFocus(face != PopupFace::standard ? popup : GetNextDlgTabItem(popup, nullptr, FALSE));
    RedrawWindow(popup, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
    menu_mark(MenuStage::painted);
    if (host.refresh_) {
        pending_file_popup = popup;
        file_preview_ready();
    }
    MSG msg{};
    while (!host.done_ && IsWindow(popup) && IsWindow(owner)) {
        int result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            if (!result)
                PostQuitMessage(int(msg.wParam));
            break;
        }
        bool inside = msg.hwnd == popup || IsChild(popup, msg.hwnd);
        if (!inside &&
            (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN || msg.message == WM_NCLBUTTONDOWN)) {
            host.done_ = true;
            // Consume this press so dismissing a menu cannot also invoke the underlying card.
            continue;
        }
        if (inside && host.key(msg))
            continue;
        if (!inside || !IsDialogMessageW(popup, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (return_focus) {
        if (GetForegroundWindow() == popup && IsWindow(owner)) return_focus(owner, host.choice_);
    } else {
        focus.restore(popup, owner);
    }
    if (IsWindow(popup))
        DestroyWindow(popup);
    return host.choice_;
}
void PopupMenuHost::size_for_labels() {
    if (!window_) return;
    HDC dc = GetDC(window_);
    const auto previous = SelectObject(dc, theme_.get(Font::menu));
    auto measured = [&](const UiText& text) {
        const auto value = text.view();
        SIZE size{};
        GetTextExtentPoint32W(dc, value.data(), int(value.size()), &size);
        return dip(window_, size.cx);
    };
    int width = base_width_;
    for (const auto& item : items_) {
        if (!item.label.localized() || (item.kind != PopupKind::command && item.kind != PopupKind::toggle)) continue;
        const auto label = item.label.display(true, true);
        const int trailing = item.kind == PopupKind::toggle ? 42 : item.hint.empty() ? 0 : measured(item.hint) + 12;
        width = std::max(width, measured(label) + trailing + (tray() ? 42 : 62));
        if (item.detail.localized()) width = std::max(width, measured(item.detail) + trailing + 62);
    }
    SelectObject(dc, previous);
    ReleaseDC(window_, dc);
    if (width == width_) return;
    width_ = width;
    const auto bounds = placement(owner_, anchor_, exclusion_ ? &*exclusion_ : nullptr);
    SetWindowPos(window_, nullptr, bounds.left, bounds.top, bounds.right-bounds.left, bounds.bottom-bounds.top,
                 SWP_NOACTIVATE | SWP_NOZORDER);
}
void PopupMenuHost::file_preview_ready() {
    if (pending_file_popup) PostMessageW(pending_file_popup, refresh_file, 0, 0);
}
void PopupMenuHost::refresh_commands() {
    if (!refresh_ || done_ || saving_.pending()) return;
    if (GetCapture() && IsChild(window_, GetCapture())) {
        SetTimer(window_, refresh_file, 50, nullptr);
        return;
    }
    try {
        if (!refresh_(subtitle_, items_)) return;
    } catch (const std::exception&) {
        done_ = true;
        return;
    }
    refresh_ = {};
    pending_file_popup = nullptr;
    int focus = GetDlgCtrlID(GetFocus());
    SetFocus(window_);
    layout_ready_ = false;
    while (auto h = GetWindow(window_, GW_CHILD)) DestroyWindow(h);
    const auto r = placement(window_, anchor_);
    if (IsRectEmpty(&r)) { done_ = true; return; }
    SetWindowPos(window_, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    set_ui_text(window_, (title_ + L" — " + subtitle_));
    create_card();
    if (frame_.window()) frame_.fit(window_, theme_.colors, design_dpi(GetDpiForWindow(window_)));
    if (auto child = GetDlgItem(window_, focus)) SetFocus(child);
}
void PopupMenuHost::create() {
    theme_.attach(window_);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_BAR_CLASSES};
    InitCommonControlsEx(&controls);
    if (tray()) {
        create_tray();
        return;
    }
    if (rope()) {
        create_rope();
        return;
    }
    if (card()) {
        create_card();
        return;
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        auto& item = items_[i];
        DWORD flags = WS_TABSTOP | (item.kind == PopupKind::toggle ? BS_AUTOCHECKBOX : BS_PUSHBUTTON);
        const bool command = item.kind == PopupKind::command || item.kind == PopupKind::toggle;
        // A native Static immediately before the Edit also labels its UI Automation Value provider.
        auto button = child(window_, command ? L"BUTTON" : L"STATIC", item.label,
                            command ? flags : SS_NOTIFY | SS_NOPREFIX, item.id);
        theme_.style(button, {item.kind == PopupKind::toggle ? Button::toggle
                              : item.danger                  ? Button::danger
                                                             : Button::command,
                              item.icon, false, false, 0, item.literal});
        if (item.literal && item.kind == PopupKind::command)
            theme_.card_style(button, {CardPart::folder_choice, item.icon, item.label});
        EnableWindow(button, item.enabled);
        if (item.kind == PopupKind::toggle)
            SendMessageW(button, BM_SETCHECK, item.checked ? BST_CHECKED : BST_UNCHECKED, 0);
        if (item.kind == PopupKind::command || item.kind == PopupKind::toggle)
            continue;
        auto edit = child(window_, L"EDIT", item.value,
                          ES_AUTOHSCROLL | WS_TABSTOP | (item.enabled ? 0 : ES_READONLY), field_base + item.id);
        theme_.field(edit);
        accessible_name(edit, item.label);
        SendMessageW(edit, EM_SETLIMITTEXT, item.kind == PopupKind::text ? 256 : 32, 0);
        SendMessageW(edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
                     MAKELPARAM(px(window_, 6), px(window_, 5)));
        if (item.kind == PopupKind::number) {
            for (int j = 0; j < 2; ++j) {
                auto b = child(window_, L"BUTTON", message(j ? TextKey::common_increase : TextKey::common_decrease, {{"label", item.label}}),
                               WS_TABSTOP, extra(i, j));
                accessible_name(b, message(j ? TextKey::common_increase : TextKey::common_decrease, {{"label", item.label}}));
                theme_.style(b, {Button::normal, j ? wchar_t(Icon::add) : wchar_t(Icon::remove), false, true});
                EnableWindow(b, item.enabled);
            }
        }
        if (item.kind == PopupKind::swatches) {
            for (size_t j = 0; j < item.palette.size(); ++j) {
                auto label = hex(item.palette[j]);
                auto b = child(window_, L"BUTTON", (item.label + L" " + label),
                               WS_TABSTOP | BS_RADIOBUTTON, extra(i, int(j)));
                SendMessageW(b, BM_SETCHECK, label == item.value ? BST_CHECKED : BST_UNCHECKED, 0);
                theme_.style(b, {Button::swatch, 0, label == item.value, false, item.palette[j]});
                accessible_name(b, item.label + L" " + label);
                EnableWindow(b, item.enabled);
            }
        }
        if (item.kind == PopupKind::opacity) {
            auto slider =
                child(window_, TRACKBAR_CLASSW, tr(TextKey::card_opacity), WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, extra(i, 0));
            accessible_name(slider, item.label);
            SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
            SendMessageW(slider, TBM_SETPOS, TRUE, _wtoi(item.value.c_str()));
        }
    }
    status_ = child(window_, L"STATIC", L"", SS_NOPREFIX, footer);
    theme_.font(status_, Font::detail);
    layout_ready_ = true;
    layout();
}

void PopupMenuHost::create_card() {
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& item = items_[i];
        const bool command = item.kind == PopupKind::command, toggle = item.kind == PopupKind::toggle;
        auto label = child(window_, L"BUTTON", item.label,
                           WS_TABSTOP | (toggle ? BS_AUTOCHECKBOX : BS_PUSHBUTTON), item.id);
        const UiText visible = item.literal ? item.label : item.label.display(false, true);
        theme_.card_style(label, {command  ? CardPart::action
                                  : toggle ? CardPart::toggle
                                           : CardPart::label,
                                  item.icon, visible, item.detail, item.hint, 0, item.preferred, false, false, item.chevron});
        EnableWindow(label, item.enabled);
        if (toggle)
            SendMessageW(label, BM_SETCHECK, item.checked ? BST_CHECKED : BST_UNCHECKED, 0);
        if (command || toggle)
            continue;
        if (item.kind == PopupKind::color) {
            auto h = child(window_, L"BUTTON", item.label, WS_TABSTOP, extra(i, 0));
            color_picker_button(theme_, h, item.color, item.color_default, ColorButtonStyle::compact, item.color_alpha);
            continue;
        }
        auto field = child(window_, L"EDIT", item.value, WS_TABSTOP | ES_AUTOHSCROLL | ES_CENTER,
                           field_base + item.id);
        ShowWindow(field, SW_HIDE);
        theme_.font(field, Font::menu);
        accessible_name(field, item.label);
        SendMessageW(field, EM_SETLIMITTEXT, 32, 0);
        if (item.kind == PopupKind::swatches) {
            tips_.bind(window_,label,[] { return tr(TextKey::card_custom_color); });
            for (size_t j = 0; j < item.palette.size(); ++j)
                child(window_, L"BUTTON", L"", WS_TABSTOP | BS_RADIOBUTTON, extra(i, int(j)));
            style_swatches(i);
            continue;
        }
        const bool size = item.kind == PopupKind::number;
        auto value = child(window_, L"BUTTON", item.label, WS_TABSTOP, extra(i, 8));
        accessible_name(value, message(TextKey::common_exact, {{"label", item.label}}));
        theme_.card_style(value, {size ? CardPart::size_value : CardPart::percent_value, 0,
                                  item.value + (size ? L" px" : L"%")});
        if (size) {
            for (int part = 0; part < 2; ++part) {
                auto b = child(window_, L"BUTTON", part ? tr(TextKey::card_font_increase) : tr(TextKey::card_font_decrease),
                               WS_TABSTOP, extra(i, part));
                theme_.card_style(b, {part ? CardPart::increase : CardPart::decrease, 0, part ? L"A+" : L"A−"});
            }
        } else {
            auto slider =
                child(window_, TRACKBAR_CLASSW, tr(TextKey::card_opacity), WS_TABSTOP | TBS_HORZ | TBS_NOTICKS, extra(i, 0));
            accessible_name(slider, item.label);
            SendMessageW(slider, TBM_SETRANGE, FALSE, MAKELPARAM(0, 100));
            SendMessageW(slider, TBM_SETPOS, TRUE, _wtoi(item.value.c_str()));
            theme_.card_style(slider, {CardPart::slider});
        }
    }
    status_ = child(window_, L"STATIC", L"", SS_NOPREFIX, footer);
    theme_.font(status_, Font::menu);
    layout_ready_ = true;
    layout_card();
}
void PopupMenuHost::style_swatches(size_t i) {
    auto& item = items_[i];
    for (size_t j = 0; j < item.palette.size(); ++j) {
        auto h = GetDlgItem(window_, extra(i, int(j)));
        auto c = displayed_color(item, j);
        auto rgb = hex(c);
        const bool selected = _wcsicmp(item.value.c_str(), rgb.c_str()) == 0;
        set_ui_text(h, (item.label + L" " + rgb));
        SendMessageW(h, BM_SETCHECK, selected ? BST_CHECKED : BST_UNCHECKED, 0);
        theme_.card_style(h, {item.square_swatches ? CardPart::tile : CardPart::dot, 0, {}, {}, {}, c, selected,
                              rope()});
    }
}
void PopupMenuHost::show_field(size_t i) {
    const auto& item = items_[i];
    auto field = GetDlgItem(window_, field_base + item.id);
    if (!field)
        return;
    if (editing_id_ && editing_id_ != item.id)
        finish_field();
    editing_id_ = item.id;
    set_ui_text(field, item.value);
    layout_card();
    SetWindowPos(field, HWND_TOP, 0, 0, 0, 0, SWP_SHOWWINDOW | SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetFocus(field);
    SendMessageW(field, EM_SETSEL, 0, -1);
}
void PopupMenuHost::finish_field() {
    const int id = editing_id_;
    if (!id)
        return;
    editing_id_ = 0;
    SetFocus(GetDlgItem(window_, id));
    ShowWindow(GetDlgItem(window_, field_base + id), SW_HIDE);
    layout_card();
}
void PopupMenuHost::layout_card() {
    RECT client{};
    GetClientRect(window_, &client);
    layout_size_ = {client.right, client.bottom};
    const int w = dip(window_, client.right), h = dip(window_, client.bottom);
    limit_ = std::max(0, preferred_height() - h);
    scroll_ = std::clamp(scroll_, 0, limit_);
    const int end = w - 8;
    ChildLayout batch;
    const UINT dpi = design_dpi(GetDpiForWindow(window_));
    auto place = [&](HWND child, int left, int top, int width, int height, UINT visibility = 0) {
        const int x = MulDiv(left, dpi, 96), y = MulDiv(top, dpi, 96);
        batch.move(child, x, y, MulDiv(left + width, dpi, 96) - x, MulDiv(top + height, dpi, 96) - y, visibility);
    };
    int y = menu_header - scroll_;
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& item = items_[i];
        const int height = row_height(item);
        if (item.divider)
            y += menu_divider;
        const int cy = y + height / 2;
        const bool full = item.kind == PopupKind::command || item.kind == PopupKind::toggle;
        const int controls = std::min(120, w / 2 - 8), start = end - controls;
        place(GetDlgItem(window_, item.id), 6, y, full ? w - 12 : start - 9, height);
        auto field = GetDlgItem(window_, field_base + item.id);
        if (item.kind == PopupKind::color) {
            place(GetDlgItem(window_, extra(i, 0)), end - 36, cy - 16, 32, 32);
        } else if (item.kind == PopupKind::swatches) {
            const int n = int(item.palette.size());
            for (int j = 0; j < n; ++j) {
                auto dot = GetDlgItem(window_, extra(i, j));
                place(dot, start + j * controls / n, cy - 12, (j + 1) * controls / n - j * controls / n, 24,
                      editing_id_ == item.id ? SWP_HIDEWINDOW : SWP_SHOWWINDOW);
            }
            place(field, start + 1, cy - 11, controls - 2, 22);
        } else if (item.kind == PopupKind::number) {
            constexpr int side = 24, middle = 48;
            place(GetDlgItem(window_, extra(i, 0)), end - 2 * side - middle, cy - 12, side, 24);
            place(GetDlgItem(window_, extra(i, 8)), end - side - middle, cy - 12, middle, 24);
            place(GetDlgItem(window_, extra(i, 1)), end - side, cy - 12, side, 24);
            place(field, end - side - middle + 3, cy - 10, middle - 6, 21);
        } else if (item.kind == PopupKind::opacity) {
            place(GetDlgItem(window_, extra(i, 0)), start, cy - 12, controls - 40, 24);
            place(GetDlgItem(window_, extra(i, 8)), end - 38, cy - 12, 38, 24);
            place(field, end - 38, cy - 10, 38, 21);
        }
        y += height;
    }
    place(status_, 12, y + 4, w - 24, error_height(), message_.empty() ? SWP_HIDEWINDOW : SWP_SHOWWINDOW);
    batch.apply(window_, limit_ > 0);
}
void PopupMenuHost::layout() {
    if (!layout_ready_)
        return;
    if (tray()) {
        layout_tray();
        return;
    }
    if (rope()) {
        layout_rope();
        return;
    }
    if (card()) {
        layout_card();
        return;
    }
    RECT rect{};
    GetClientRect(window_, &rect);
    const int h = dip(window_, rect.bottom), width = dip(window_, rect.right);
    limit_ = std::max(0, preferred_height() - h);
    scroll_ = std::clamp(scroll_, 0, limit_);
    ChildLayout batch;
    int y = 76 - scroll_;
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& item = items_[i];
        if (item.divider)
            y += 9;
        const int height = row_height(item);
        const bool full = item.kind == PopupKind::command || item.kind == PopupKind::toggle;
        const int controls = std::min(198, width / 2);
        const int x = width - controls - 16;
        batch.place(GetDlgItem(window_, item.id), 8, y, full ? width - 16 : x - 12, height - 3);
        const int field_y = y + (height - 28) / 2;
        auto field = GetDlgItem(window_, field_base + item.id);
        if (item.kind == PopupKind::number) {
            batch.place(GetDlgItem(window_, extra(i, 0)), x, field_y, 32, 28);
            batch.place(field, x + 36, field_y, controls - 72, 28);
            batch.place(GetDlgItem(window_, extra(i, 1)), width - 48, field_y, 32, 28);
        } else if (item.kind == PopupKind::swatches) {
            int dot = std::min(24, (controls - 68) / std::max(1, int(item.palette.size())));
            for (size_t j = 0; j < item.palette.size(); ++j)
                batch.place(GetDlgItem(window_, extra(i, int(j))), x + int(j) * dot, field_y, dot, 26);
            int start = x + int(item.palette.size()) * dot + 4;
            batch.place(field, start, field_y, width - 16 - start, 28);
        } else if (item.kind == PopupKind::opacity) {
            batch.place(GetDlgItem(window_, extra(i, 0)), x, field_y, controls - 64, 28);
            batch.place(field, width - 74, field_y, 58, 28);
        } else if (field)
            batch.place(field, x, field_y, controls, 28);
        y += height;
    }
    batch.place(status_, 18, y + 4, width - 36, 52, message_.empty() ? SWP_HIDEWINDOW : SWP_SHOWWINDOW);
    batch.apply(window_, limit_ > 0);
}
void PopupMenuHost::paint(HDC dc) {
    if (tray()) {
        paint_tray(dc);
        return;
    }
    if (rope()) {
        paint_rope(dc);
        return;
    }
    RECT r{};
    GetClientRect(window_, &r);
    const auto& p = theme_.colors;
    if (card()) {
        solid(dc, r, p.panel);
        RECT head{px(window_, 6), px(window_, 2 - scroll_), r.right - px(window_, 6), px(window_, menu_header - 5 - scroll_)};
        theme_.draw_card(window_, dc, head, {CardPart::header, 0, title_, subtitle_});
        RECT rule{px(window_, 8), px(window_, menu_header - 3 - scroll_), r.right - px(window_, 8),
                  px(window_, menu_header - 3 - scroll_) + 1};
        solid(dc, rule, p.line);
        return;
    }
    round_box(dc, r, px(window_, 9), p.panel, p.line);
    auto band = [&](int y, int height) {
        return RECT{px(window_, 18), px(window_, y - scroll_), r.right - px(window_, 18),
                    px(window_, y + height - scroll_)};
    };
    RECT header{px(window_, 8), px(window_, 2 - scroll_), r.right - px(window_, 8), px(window_, 65 - scroll_)};
    theme_.menu_header(dc, header, title_, subtitle_, items_.front().icon);
    int top = 76;
    solid(dc, band(68, 1), p.line);
    for (auto& item : items_) {
        if (item.divider) {
            solid(dc, band(top + 3, 1), p.line);
            top += 9;
        }
        top += row_height(item);
    }
}
void PopupMenuHost::submit(size_t index, std::wstring value) {
    if (saving_.pending()) return;
    write([this, index, value = std::move(value)]() mutable { submit_value(index, std::move(value)); });
}
void PopupMenuHost::write(std::function<void()> commit) {
    if (saving_.pending() || done_) return;
    const bool queued = saving_.submit(defer_history_, [this, commit = std::move(commit)] {
        if (!done_ && IsWindow(owner_)) commit();
    }, [this](auto error) {
        if (error) { message_ = tr(TextKey::error_item_save); update_status(); }
        if (refresh_ && !done_) PostMessageW(window_, refresh_file, 0, 0);
    }, true);
    if (queued) {
        message_ = tr(TextKey::common_saving);
        update_status();
        NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, status_, OBJID_CLIENT, CHILDID_SELF);
    }
}
void PopupMenuHost::update_status() {
    const bool was_visible = (GetWindowLongPtrW(status_, GWL_STYLE) & WS_VISIBLE) != 0;
    set_ui_text(status_, message_);
    if ((!message_.empty() || was_visible) && (card() || rope())) {
        const auto bounds = placement(owner_, anchor_, exclusion_ ? &*exclusion_ : nullptr);
        RECT previous{};
        GetWindowRect(window_, &previous);
        if (!IsRectEmpty(&bounds) && !EqualRect(&bounds, &previous)) {
            SetWindowPos(window_, nullptr, bounds.left, bounds.top, bounds.right - bounds.left,
                         bounds.bottom - bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
            if (frame_.window()) frame_.fit(window_, theme_.colors, design_dpi(GetDpiForWindow(window_)));
        }
    }
    if (!message_.empty()) scroll_ = preferred_height();
    layout();
}
void PopupMenuHost::submit_value(size_t index, std::wstring value) {
    auto& item = items_[index];
    if (!item.enabled || !item.commit)
        return;
    if (message_.empty() && value == item.value) {
        if (card() && editing_id_ == item.id)
            finish_field();
        return;
    }
    PopupResult result{};
    try {
        result = item.commit(value);
    } catch (...) {
        result.error = tr(TextKey::error_item_save);
    }
    message_ = result.error;
    if (message_.empty())
        item.value = std::move(value);
    else
        message_ = message(TextKey::popup_saved_value, {{"error", message_}, {"value", item.value}});
    if (result.dismiss && message_.empty()) {
        done_ = true;
        return;
    }
    auto field = GetDlgItem(window_, field_base + item.id);
    if (message_.empty() && field)
        set_ui_text(field, item.value);
    if (item.kind == PopupKind::opacity)
        SendDlgItemMessageW(window_, extra(index, 0), TBM_SETPOS, TRUE, _wtoi(item.value.c_str()));
    if ((card() || rope()) && item.kind == PopupKind::swatches)
        style_swatches(index);
    if (rope() && item.kind == PopupKind::text && message_.empty()) {
        subtitle_ = item.value;
        set_ui_text(window_, (title_ + L" — " + subtitle_));
    }
    if (card()) {
        if (item.kind == PopupKind::number || item.kind == PopupKind::opacity) {
            auto formatted = item.value + (item.kind == PopupKind::number ? L" px" : L"%");
            theme_.card_style(
                GetDlgItem(window_, extra(index, 8)),
                {item.kind == PopupKind::number ? CardPart::size_value : CardPart::percent_value, 0, formatted});
        }
        if (message_.empty() && editing_id_ == item.id)
            finish_field();
    } else if (!rope())
        for (size_t j = 0; j < item.palette.size(); ++j) {
            auto swatch = GetDlgItem(window_, extra(index, int(j)));
            const bool selected = _wcsicmp(item.value.c_str(), hex(item.palette[j]).c_str()) == 0;
            SendMessageW(swatch, BM_SETCHECK, selected ? BST_CHECKED : BST_UNCHECKED, 0);
            theme_.style(swatch, {Button::swatch, 0, selected, false, item.palette[j]});
        }
    update_status();
}
void PopupMenuHost::handle(int id) {
    if (saving_.pending()) return;
    for (size_t i = 0; i < items_.size(); ++i) {
        auto& item = items_[i];
        if (!item.enabled)
            continue;
        if (item.kind == PopupKind::color && (item.id == id || id == extra(i, 0))) {
            try {
                item.color = item.pick_color(window_, GetDlgItem(window_, extra(i, 0)));
            } catch (...) {
                message_ = tr(TextKey::error_item_save);
                update_status();
            }
            color_picker_button(theme_, GetDlgItem(window_, extra(i, 0)), item.color, item.color_default,
                                ColorButtonStyle::compact, item.color_alpha);
            layout();
            return;
        }
        if (item.id == id) {
            if (item.kind == PopupKind::command || item.kind == PopupKind::segments ||
                (item.kind == PopupKind::toggle && !item.commit_toggle)) {
                choice_ = id;
                done_ = true;
                return;
            }
            if (item.kind == PopupKind::toggle) {
                write([this, i, proposed = !item.checked] { submit_toggle(i, proposed); });
            }
            else if (card())
                show_field(i);
            else {
                auto field = GetDlgItem(window_, field_base + id);
                if (field) {
                    SetFocus(field);
                    SendMessageW(field, EM_SETSEL, 0, -1);
                }
            }
            return;
        }
        int part = id - extra(i, 0);
        if (part < 0 || part >= 10)
            continue;
        if (item.kind == PopupKind::segments && part < 2) {
            if (item.selection != part) {
                choice_ = item.id;
                done_ = true;
            }
            return;
        }
        if (card() && part == 8) {
            show_field(i);
            return;
        }
        if (item.kind == PopupKind::swatches && part < int(item.palette.size()))
            submit(i, hex((card() || rope()) ? displayed_color(item, part) : item.palette[part]));
        if (item.kind == PopupKind::number && part < 2) {
            wchar_t value[48]{};
            swprintf_s(value, L"%.10g", _wtof(item.value.c_str()) + (part ? 1 : -1));
            submit(i, value);
        }
        return;
    }
}
void PopupMenuHost::submit_toggle(size_t index, bool proposed) {
    auto& item = items_.at(index);
    PopupResult result;
    try { result = item.commit_toggle(proposed); }
    catch (...) { result.error = tr(TextKey::error_item_save); }
    message_ = std::move(result.error);
    if (message_.empty()) item.checked = proposed;
    SendDlgItemMessageW(window_, item.id, BM_SETCHECK, item.checked ? BST_CHECKED : BST_UNCHECKED, 0);
    update_status();
    InvalidateRect(GetDlgItem(window_, item.id), nullptr, FALSE);
}
bool PopupMenuHost::key(MSG& msg) {
    if (msg.message != WM_KEYDOWN)
        return false;
    if (tray() && tray_key(msg.wParam))
        return true;
    auto focused = GetFocus();
    if (focused && (msg.wParam == VK_ESCAPE || msg.wParam == VK_RETURN)) {
        HIMC imc = ImmGetContext(focused);
        const bool composing = imc && ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0) > 0;
        if (imc)
            ImmReleaseContext(focused, imc);
        if (composing) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            return true;
        }
    }
    if (msg.wParam == VK_ESCAPE) {
        done_ = true;
        return true;
    }
    HWND focus = GetFocus();
    const int id = GetDlgCtrlID(focus);
    if (msg.wParam == VK_RETURN) {
        if ((card() || rope() || tray()) && focus == window_) {
            auto first =
                std::find_if(items_.begin(), items_.end(), [](const PopupItem& item) { return item.preferred; });
            if (first != items_.end())
                handle(first->id);
            else if (!items_.empty())
                handle(items_.front().id);
            return true;
        }
        for (size_t i = 0; i < items_.size(); ++i)
            if (id == field_base + items_[i].id) {
                submit(i, caption(focus));
                return true;
            }
        if (focus && IsWindowEnabled(focus))
            SendMessageW(focus, BM_CLICK, 0, 0);
        return true;
    }
    if (msg.wParam == VK_UP || msg.wParam == VK_DOWN || msg.wParam == VK_TAB) {
        const bool previous = msg.wParam == VK_UP || (msg.wParam == VK_TAB && GetKeyState(VK_SHIFT) < 0);
        if (auto next = GetNextDlgTabItem(window_, focus, previous)) {
            if (card() || rope() || tray())
                SendMessageW(window_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
            SetFocus(next);
            RECT bounds{};
            GetWindowRect(next, &bounds);
            MapWindowPoints(nullptr, window_, reinterpret_cast<POINT*>(&bounds), 2);
            RECT client{};
            GetClientRect(window_, &client);
            const int before = scroll_;
            if (bounds.top < 0)
                scroll_ += dip(window_, bounds.top) - 4;
            if (bounds.bottom > client.bottom)
                scroll_ += dip(window_, bounds.bottom - client.bottom) + 4;
            if (scroll_ != before) layout();
        }
        return true;
    }
    return false;
}
LRESULT CALLBACK PopupMenuHost::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto self = reinterpret_cast<PopupMenuHost*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<PopupMenuHost*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->window_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(self));
    }
    if (!self)
        return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_TIMER:
        if (w != refresh_file) break;
        KillTimer(h, refresh_file);
        [[fallthrough]];
    case refresh_file:
        self->refresh_commands();
        return 0;
    case menu_timing_query:
#ifdef CLIPROPE_DESKTOP_PROBES
        if (w == static_cast<WPARAM>(MenuStage::count) + 1) return Surface::target_creations();
        if (w == static_cast<WPARAM>(MenuStage::count) + 2) return CardMenuFrame::generations();
#endif
        if (w == static_cast<WPARAM>(MenuStage::count)) {
            if (!menu_times.back().QuadPart || FAILED(DwmFlush())) return 0;
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            return now.QuadPart;
        }
        return w < menu_times.size() ? menu_times[w].QuadPart : 0;
    case WM_CREATE:
        self->create();
        return 0;
    case WM_ACTIVATE:
        // UI Automation can activate the target child before it invokes the button.
        if (LOWORD(w) == WA_INACTIVE && !IsChild(h, reinterpret_cast<HWND>(l)) &&
            !ColorPickerPopup::owns(HWND(l))) {
            self->done_ = true;
            PostMessageW(h, WM_NULL, 0, 0);
        }
        break;
    case WM_CLOSE:
        self->done_ = true;
        PostMessageW(h, WM_NULL, 0, 0);
        return 0;
    case WM_COMMAND:
        if (self->rope() && (HIWORD(w) == EN_SETFOCUS || HIWORD(w) == EN_KILLFOCUS)) {
            if (HIWORD(w) == EN_KILLFOCUS)
                for (const auto& item : self->items_)
                    if (LOWORD(w) == field_base + item.id)
                        set_ui_text(HWND(l), item.value);
            RECT field{};
            GetWindowRect(HWND(l), &field);
            MapWindowPoints(nullptr, h, reinterpret_cast<POINT*>(&field), 2);
            InflateRect(&field, px(h, 6), px(h, 6));
            InvalidateRect(h, &field, FALSE);
            return 0;
        }
        if (LOWORD(w) == IDCANCEL)
            self->done_ = true;
        else if (self->editing_id_ && HIWORD(w) == EN_KILLFOCUS && LOWORD(w) == field_base + self->editing_id_) {
            self->editing_id_ = 0;
            ShowWindow(HWND(l), SW_HIDE);
            self->layout();
        } else if (HIWORD(w) == BN_CLICKED)
            self->handle(LOWORD(w));
        if (self->done_)
            PostMessageW(h, WM_NULL, 0, 0);
        return 0;
    case WM_NOTIFY: {
        if (self->card())
            break;
        auto& n = *reinterpret_cast<NMHDR*>(l);
        if (n.code != NM_CUSTOMDRAW)
            break;
        auto& draw = *reinterpret_cast<NMCUSTOMDRAW*>(l);
        if (draw.dwDrawStage == CDDS_PREPAINT)
            return CDRF_NOTIFYITEMDRAW;
        if (draw.dwDrawStage != CDDS_ITEMPREPAINT)
            break;
        auto& p = self->theme_.colors;
        RECT r = draw.rc;
        if (draw.dwItemSpec == TBCD_CHANNEL) {
            int mid = (r.top + r.bottom) / 2;
            r.top = mid - px(h, 2);
            r.bottom = mid + px(h, 2);
            round_box(draw.hdc, r, px(h, 2), p.subtle, p.line);
            r.right = r.left + MulDiv(r.right - r.left, int(SendMessageW(n.hwndFrom, TBM_GETPOS, 0, 0)), 100);
            round_box(draw.hdc, r, px(h, 2), p.accent, p.accent);
            return CDRF_SKIPDEFAULT;
        }
        if (draw.dwItemSpec == TBCD_THUMB) {
            int midx = (r.left + r.right) / 2, midy = (r.top + r.bottom) / 2, rad = px(h, 7);
            r = {midx - rad, midy - rad, midx + rad, midy + rad};
            round_box(draw.hdc, r, rad, p.panel, GetFocus() == n.hwndFrom ? p.accent : p.muted);
            return CDRF_SKIPDEFAULT;
        }
        break;
    }
    case WM_HSCROLL:
        if (self->card() && LOWORD(w) == TB_THUMBTRACK) {
            for (size_t i = 0; i < self->items_.size(); ++i)
                if (self->items_[i].kind == PopupKind::opacity && HWND(l) == GetDlgItem(h, extra(i, 0))) {
                    const auto text = std::to_wstring(SendMessageW(HWND(l), TBM_GETPOS, 0, 0)) + L"%";
                    self->theme_.card_style(GetDlgItem(h, extra(i, 8)), {CardPart::percent_value, 0, text});
                    InvalidateRect(HWND(l), nullptr, FALSE);
                }
        }
        if (LOWORD(w) == TB_ENDTRACK || LOWORD(w) == TB_LINEUP || LOWORD(w) == TB_LINEDOWN ||
            LOWORD(w) == TB_PAGEUP || LOWORD(w) == TB_PAGEDOWN) {
            for (size_t i = 0; i < self->items_.size(); ++i)
                if (self->items_[i].kind == PopupKind::opacity && GetDlgCtrlID(HWND(l)) == extra(i, 0))
                    self->submit(i, std::to_wstring(SendMessageW(HWND(l), TBM_GETPOS, 0, 0)));
        }
        return 0;
    case WM_MOUSEWHEEL:
        self->scroll_ -= GET_WHEEL_DELTA_WPARAM(w) * 48 / WHEEL_DELTA;
        self->layout();
        return 0;
    case WM_SIZE:
        if (self->layout_size_.cx != LOWORD(l) || self->layout_size_.cy != HIWORD(l)) self->layout();
        return 0;
    case WM_DPICHANGED: {
        auto& r = *reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER);
        [[fallthrough]];
    }
    case language_changed:
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
    case WM_FONTCHANGE:
    case WM_DISPLAYCHANGE:
        fluent::Surface::clear_control_artwork();
        if (m != language_changed) self->theme_.refresh();
        self->size_for_labels();
        if (self->frame_.window())
            self->frame_.fit(h, self->theme_.colors, design_dpi(GetDpiForWindow(h)));
        self->layout();
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return self->theme_.control_color(HDC(w), HWND(l), true);
    case WM_ERASEBKGND:
        return 1;
    case WM_PRINTCLIENT:
        self->paint(reinterpret_cast<HDC>(w));
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        self->paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace cliprope::win
