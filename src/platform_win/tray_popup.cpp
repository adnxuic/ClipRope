#include "platform_win/popup_menu.hpp"
#include "platform_win/tray_popup.hpp"
#include <algorithm>
#include <imm.h>

namespace cliprope::win {
using namespace fluent;
namespace {
constexpr int inset = 5, header_top = 5, header_bottom = 33;
constexpr int band_top = 37, band_pad = 3, row_height_dip = 21;
struct TrayLayout {
    explicit TrayLayout(const std::vector<PopupItem>& items) {
        const bool hidden = std::any_of(items.begin(),items.end(),[](const auto& row){return row.id == tray_command::reveal_hidden;});
        band_bottom = band_top + 2 * band_pad + (int(items.size()) - 4 - int(hidden)) * row_height_dip;
        first_rule = band_bottom + 4;
        note_bottom = first_rule + 25;
        settings_rule = note_bottom + (hidden ? 25 : 0);
        last_rule = settings_rule + 25;
    }
    int band_bottom, first_rule, note_bottom, settings_rule, last_rule;
};
} // namespace

int PopupMenuHost::tray_height() const { return TrayLayout(items_).last_rule + 26; }

void PopupMenuHost::create_tray() {
    for (const auto& item : items_) {
        CardPart part = CardPart::tray_preview;
        UiText name = item.hint.empty() ? item.label : item.hint + L" " + item.label;
        if (item.id == tray_command::open) {
            part = CardPart::tray_header;
            name = item.detail + L": " + item.label;
        } else if (item.id == tray_command::quick_note) {
            part = CardPart::tray_quick_note;
            name = item.label + L" " + item.hint;
        } else if (item.id == tray_command::reveal_hidden) {
            part = CardPart::action;
        } else if (item.id == tray_command::settings) {
            part = CardPart::tray_settings;
        } else if (item.id == tray_command::exit) {
            part = CardPart::tray_exit;
        }
        // Dialog keyboard handling must never turn ampersands in saved text into mnemonics.
        auto control = child(window_, L"BUTTON", name.native_literal(), WS_TABSTOP | BS_PUSHBUTTON, item.id);
        accessible_name(control, name);
        theme_.card_style(control, {part, item.icon, item.label, item.detail, item.hint});
        EnableWindow(control, item.enabled);
    }
    // This command-only surface uses letters as menu keys, including with a Chinese IME active.
    ImmAssociateContextEx(window_, nullptr, IACE_CHILDREN);
    layout_ready_ = true;
    layout_tray();
}

void PopupMenuHost::layout_tray() {
    RECT r{};
    GetClientRect(window_, &r);
    layout_size_ = {r.right, r.bottom};
    const TrayLayout metrics(items_);
    limit_ = std::max(0, tray_height() - dip(window_, r.bottom));
    scroll_ = std::clamp(scroll_, 0, limit_);
    ChildLayout batch;
    int top = band_top + band_pad;
    for (const auto& item : items_) {
        int x = inset, y, bottom;
        switch (item.id) {
        case tray_command::open: y = header_top; bottom = header_bottom; break;
        case tray_command::quick_note: y = metrics.first_rule + 1; bottom = metrics.note_bottom; break;
        case tray_command::reveal_hidden: y = metrics.note_bottom; bottom = metrics.settings_rule; break;
        case tray_command::settings: y = metrics.settings_rule + 1; bottom = metrics.last_rule; break;
        case tray_command::exit: y = metrics.last_rule + 1; bottom = tray_height() - 3; break;
        default:
            x += band_pad;
            y = top;
            top += row_height_dip;
            bottom = top;
        }
        const int left = px(window_, x), start = px(window_, y - scroll_);
        batch.move(GetDlgItem(window_, item.id), left, start, r.right - 2 * left, px(window_, bottom - scroll_) - start);
    }
    batch.apply(window_, limit_ > 0);
}

void PopupMenuHost::paint_tray(HDC dc) {
    RECT client{};
    GetClientRect(window_, &client);
    solid(dc, client, theme_.colors.panel);
    const TrayLayout metrics(items_);
    const int x = px(window_, inset);
    auto strip = [&](int top, int bottom) -> RECT {
        return {x, px(window_, top - scroll_), client.right - x, px(window_, bottom - scroll_)};
    };
    theme_.draw_card(window_, dc, strip(band_top, metrics.band_bottom), {CardPart::tray_band});
    for (int rule : {metrics.first_rule, metrics.settings_rule, metrics.last_rule})
        solid(dc, strip(rule, rule + 1), theme_.colors.line);
}

bool PopupMenuHost::tray_key(WPARAM key) {
    if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0) return false;
    if (key >= VK_NUMPAD1 && key <= VK_NUMPAD5) key = '1' + key - VK_NUMPAD1;
    if (key >= '1' && key <= '5')
        handle(tray_command::first_preview + int(key - '1'));
    else if (key == 'H') {
        if (GetDlgItem(window_, tray_command::first_preview)) {
            choice_ = tray_command::head;
            done_ = true;
        }
    } else if (key == 'S')
        handle(tray_command::settings);
    else if (key == 'X')
        handle(tray_command::exit);
    else
        return false;
    return true;
}
} // namespace cliprope::win
