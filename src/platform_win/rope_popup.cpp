#include "platform_win/popup_menu.hpp"
#include "platform_win/color_picker.hpp"
#include "platform_win/workspace_commands.hpp"
#include <algorithm>

namespace cliprope::win {
using namespace fluent;
namespace {
// DIP offsets below the header. The final two rows include a four-DIP divider.
constexpr int edges[] = {0, 56, 80, 108, 132, 156, 184, 212, 236, 264, 288, 316};
constexpr int field_offset = 25, field_height = 24;
struct StyleColumns {
    explicit StyleColumns(int w) : end(w - 12), step(end - 80), dots(step - 4 - 36), name_right(dots - 4) {}
    int end, step, dots, name_right;
    static constexpr int name_left = 12;
};
bool overview_row(const PopupItem& item) { return item.id == int(WorkspaceCommand::overview); }
} // namespace

int PopupMenuHost::rope_body_height() const {
    const int reason = items_.back().detail.empty() ? 0 : 12;
    return menu_header + edges[11] + reason + menu_bottom;
}

void PopupMenuHost::create_rope() {
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& item = items_[i];
        if (item.kind == PopupKind::text || item.kind == PopupKind::number) {
            const bool number = item.kind == PopupKind::number;
            auto field = child(window_, L"EDIT", item.value, WS_TABSTOP | ES_AUTOHSCROLL |
                               (number ? ES_CENTER : ES_LEFT) | (item.enabled ? 0 : ES_READONLY), field_base + item.id);
            theme_.font(field, Font::menu);
            accessible_name(field, item.label);
            if (number) SendMessageW(field, EM_SETLIMITTEXT, 10, 0);
            if (number && item.enabled) {
                for (int part = 0; part < 2; ++part) {
                    auto button = child(window_, L"BUTTON", part ? tr(TextKey::rope_capacity_increase) : tr(TextKey::rope_capacity_decrease),
                                        WS_TABSTOP, extra(i, part));
                    theme_.card_style(button, {part ? CardPart::increase : CardPart::decrease, 0,
                                               part ? L"+" : L"−", {}, {}, 0, false, true});
                }
            }
            continue;
        }
        if (item.kind == PopupKind::color) {
            auto h = child(window_, L"BUTTON", item.label, WS_TABSTOP, extra(i, 0));
            color_picker_button(theme_, h, item.color, item.color_default, ColorButtonStyle::compact);
            continue;
        }
        if (item.kind == PopupKind::swatches) {
            for (size_t j = 0; j < item.palette.size(); ++j)
                child(window_, L"BUTTON", L"", WS_TABSTOP | BS_RADIOBUTTON | (j ? 0 : WS_GROUP), extra(i, int(j)));
            style_swatches(i);
            continue;
        }
        const bool toggle = item.kind == PopupKind::toggle, segments = item.kind == PopupKind::segments;
        auto action = child(window_, L"BUTTON", item.label, WS_TABSTOP | WS_GROUP |
                            (toggle ? BS_CHECKBOX : BS_PUSHBUTTON), item.id);
        const UiText label = item.label.display(true, true);
        const bool band = i == 0 || overview_row(item);
        theme_.card_style(action, {i == 0 ? CardPart::rope_style : toggle ? CardPart::toggle : CardPart::rope_action, item.icon, label,
                                   item.detail, item.hint, 0, false, band, false, item.chevron});
        EnableWindow(action, item.enabled);
        if (toggle)
            SendMessageW(action, BM_SETCHECK, item.checked ? BST_CHECKED : BST_UNCHECKED, 0);
        if (segments) {
            for (int part = 0; part < 2; ++part) {
                auto option = child(window_, L"BUTTON", item.segments[part],
                                    WS_TABSTOP | BS_RADIOBUTTON | (part ? 0 : WS_GROUP), extra(i, part));
                bool selected = item.selection == part;
                SendMessageW(option, BM_SETCHECK, selected ? BST_CHECKED : BST_UNCHECKED, 0);
                theme_.card_style(option, {part ? CardPart::segment_right : CardPart::segment_left, 0,
                                           item.segments[part], {}, {}, 0, selected, band, true});
            }
        }
    }
    status_ = child(window_, L"STATIC", L"", SS_NOPREFIX, footer);
    theme_.font(status_, Font::menu);
    layout_ready_ = true;
    layout_rope();
}

void PopupMenuHost::layout_rope() {
    RECT r{};
    GetClientRect(window_, &r);
    layout_size_ = {r.right, r.bottom};
    int w = dip(window_, r.right), height = dip(window_, r.bottom);
    limit_ = std::max(0, preferred_height() - height);
    scroll_ = std::clamp(scroll_, 0, limit_);
    StyleColumns c(w);
    constexpr int field_top = menu_header + field_offset, field_bottom = field_top + field_height;
    ChildLayout batch;
    const UINT dpi = design_dpi(GetDpiForWindow(window_));
    auto scale = [dpi](int n) { return MulDiv(n, dpi, 96); };
    auto move = [&](int id, int l, int t, int right, int bottom, UINT visibility = 0) {
        if (auto h = GetDlgItem(window_, id)) {
            // Round shared edges once, including the interior seams of the segmented controls.
            int x = scale(l), y = scale(t - scroll_);
            batch.move(h, x, y, scale(right) - x, scale(bottom - scroll_) - y, visibility);
        }
    };
    size_t row = 0;
    for (size_t i = 0; i < items_.size(); ++i) {
        const auto& item = items_[i];
        switch (item.kind) {
        case PopupKind::text:
            move(field_base + item.id, c.name_left + 4, field_top + 3, c.name_right - 3, field_bottom - 2);
            continue;
        case PopupKind::color:
            move(extra(i, 0), c.dots, field_top - 4, c.dots + 32, field_top + 28);
            continue;
        case PopupKind::swatches:
            for (size_t j = 0; j < item.palette.size(); ++j)
                move(extra(i, int(j)), c.dots + int(j)*24, field_top, c.dots + int(j+1)*24, field_bottom);
            continue;
        case PopupKind::number: {
            const int side = item.enabled ? 20 : 0;
            move(extra(i, 0), c.step, field_top, c.step + side, field_bottom);
            move(extra(i, 1), c.end - side, field_top, c.end, field_bottom);
            move(field_base + item.id, c.step + side + 1, field_top + 3, c.end - side - 1, field_bottom - 2);
            continue;
        }
        default:
            break;
        }
        const int top = menu_header + edges[row] + (item.divider ? menu_divider : 0);
        const int bottom = row == 10 ? rope_body_height() - menu_bottom : menu_header + edges[row + 1];
        if (i == 0)
            move(item.id, 8, top + 1, w - 8, field_top);
        else if (item.kind == PopupKind::segments) {
            const int start = c.end - 128, mid = (start + c.end) / 2;
            const int cy = (top + bottom) / 2;
            const int inset = overview_row(item) ? 2 : 0;
            move(item.id, 8, top + inset, start - 2, bottom - inset);
            move(extra(i, 0), start, cy - 12, mid, cy + 12);
            move(extra(i, 1), mid, cy - 12, c.end, cy + 12);
        } else
            move(item.id, 6, top, w - 6, bottom);
        ++row;
    }
    const int status_top = rope_body_height() - menu_bottom + 4;
    move(footer, 12, status_top, w - 12, status_top + error_height(), message_.empty() ? SWP_HIDEWINDOW : SWP_SHOWWINDOW);
    batch.apply(window_, limit_ > 0);
}

void PopupMenuHost::paint_rope(HDC dc) {
    RECT r{};
    GetClientRect(window_, &r);
    const int w = dip(window_, r.right);
    StyleColumns c(w);
    auto rect = [&](int l, int t, int right, int bottom) -> RECT {
        return {px(window_, l), px(window_, t - scroll_), px(window_, right), px(window_, bottom - scroll_)};
    };
    solid(dc, r, theme_.colors.panel);
    theme_.draw_card(window_, dc, rect(6, 2, w - 6, menu_header - 5), {CardPart::rope_header, 0, title_, subtitle_});
    for (int y : {menu_header - 3, menu_header + edges[8] + 2, menu_header + edges[10] + 2})
        solid(dc, rect(8, y - 1, w - 8, y), theme_.colors.line);
    theme_.draw_card(window_, dc, rect(5, menu_header, w - 5, menu_header + edges[1] - 2), {CardPart::band});
    theme_.draw_card(window_, dc, rect(5, menu_header + edges[6], w - 5, menu_header + edges[7]), {CardPart::band});
    constexpr int field_top = menu_header + field_offset, field_bottom = field_top + field_height;
    solid(dc, rect(c.step - 4, field_top + 3, c.step - 3, field_bottom - 2), theme_.colors.line);
    for (const auto& item : items_) {
        if (item.kind != PopupKind::number && item.kind != PopupKind::text)
            continue;
        const bool number = item.kind == PopupKind::number;
        auto bounds = rect(number ? c.step : c.name_left, field_top, number ? c.end : c.name_right, field_bottom);
        const bool focus = GetFocus() == GetDlgItem(window_, field_base + item.id);
        theme_.draw_card(window_, dc, bounds, {CardPart::input, 0, {}, {}, {}, 0, focus, true});
    }
}
} // namespace cliprope::win
