#include "platform_win/settings.hpp"
#include "platform_win/fluent_list.hpp"
#include <algorithm>
#include <charconv>
#include <stdexcept>
#include <span>

namespace cliprope::win {
using namespace fluent;
namespace {
struct SettingRow {
    UiText group, label, description;
};
const UiText actions[]{tr(TextKey::settings_cards_paste), tr(TextKey::settings_cards_copy)};
const UiText delete_actions[]{tr(TextKey::settings_cards_delete_direct), tr(TextKey::settings_cards_delete_recycle)};
const SettingRow settings[] = {
    {tr(TextKey::settings_cards_actions), tr(TextKey::settings_cards_action), nullptr},
    {nullptr, tr(TextKey::settings_cards_follow_head), tr(TextKey::settings_cards_follow_head_description)},
    {nullptr, tr(TextKey::settings_cards_delete_hanging), tr(TextKey::settings_cards_delete_hanging_description)},
    {nullptr, tr(TextKey::settings_cards_delete_floating), nullptr},
    {tr(TextKey::card_text), tr(TextKey::settings_cards_font), tr(TextKey::settings_cards_font_description)},
    {tr(TextKey::settings_cards_images), tr(TextKey::settings_cards_resolution), nullptr},
    {nullptr, tr(TextKey::settings_cards_screenshot_preview), nullptr},
    {tr(TextKey::card_files), tr(TextKey::settings_cards_thumbnails), nullptr},
    {nullptr, tr(TextKey::settings_cards_keep_group), nullptr},
    {nullptr, tr(TextKey::settings_ropes_capacity), nullptr},
    {nullptr, tr(TextKey::settings_ropes_follow_collector), tr(TextKey::settings_ropes_follow_collector_description)}
};

template<class T> bool number(HWND source, T& out) {
    const auto input = caption(source);
    if (input.find_first_not_of(L"0123456789.") != input.npos || input.empty()) return false;
    std::string ascii;
    for (auto unit : input) ascii.push_back(static_cast<char>(unit));
    const auto result = std::from_chars(ascii.data(), ascii.data() + ascii.size(), out);
    return result.ec == std::errc{} && result.ptr == ascii.data() + ascii.size();
}
}

void Settings::card_rows() {
    static_assert(std::size(settings) == std::size(card_order) + std::size(rope_order));
    const auto order = selected_ == 1 ? std::span<const int>(card_order) : std::span<const int>(rope_order);
    for (size_t index = 0; index < order.size(); ++index) {
        const int id = order[index];
        const auto& spec = settings[(selected_ == 1 ? 0 : std::size(card_order)) + index];
        Row r{};
        if (!spec.group.empty()) heading(r, spec.group);
        r.label = child(viewport_, L"STATIC", spec.label, SS_NOPREFIX, 0);
        r.detail = child(viewport_, L"STATIC", spec.description, SS_NOPREFIX, 0);
        surface_.font(r.label);
        surface_.font(r.detail, Font::detail);
        if (card_choice(id)) {
            r.value = choice_box(surface_, viewport_, spec.label, id);
            if (!r.value) throw std::runtime_error("Could not create the card action choice.");
            fill_card_choice(id);
        } else if (id == card_font || id == rope_capacity) {
            r.value = child(viewport_, L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL | ES_CENTER, id);
            surface_.field(r.value);
        } else {
            r.value = child(viewport_, L"BUTTON", spec.label, WS_TABSTOP | BS_CHECKBOX | BS_NOTIFY, id);
            surface_.card_style(r.value, {CardPart::settings_switch});
        }
        surface_.font(r.value);
        accessible_name(r.value, spec.label);
        accessible_description(r.value, spec.description);
        EnableWindow(r.value, id == rope_capacity ? bool(data_.save_capacity) : bool(data_.save_cards));
        rows_.push_back(r);
        sync_card_control(id);
    }
}

void Settings::fill_card_choice(int id) {
    const auto h = GetDlgItem(viewport_, id);
    SendMessageW(h, CB_RESETCONTENT, 0, 0);
    for (const auto& label : (id == card_action ? actions : delete_actions))
        SendMessageW(h, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
}

void Settings::sync_card_control(int id) {
    const auto h = GetDlgItem(viewport_, id);
    const auto& p = data_.cards;
    if (card_choice(id)) {
        const int selected = id == card_action ? !p.paste_on_double_click :
                             id == card_delete_hanging ? p.recycle_hanging : p.recycle_floating;
        const auto& labels = id == card_action ? actions : delete_actions;
        SendMessageW(h, CB_SETCURSEL, selected, 0);
        surface_.card_style(h, {.part = CardPart::settings_choice, .label = labels[selected]});
    } else if (id == card_font) {
        char buf[48]{};
        const auto end = std::to_chars(buf, buf + sizeof(buf), p.font_size).ptr;
        set_ui_text(h, std::wstring(buf, end));
    } else if (id == rope_capacity) set_ui_text(h, std::to_wstring(data_.default_capacity));
    else {
        const bool on = id == card_resolution ? p.hover_resolution :
                        id == card_follow_head ? p.follow_card_head :
                        id == rope_follow_collector ? p.follow_collector :
                        id == card_thumbnails ? p.image_file_previews :
                        id == card_screenshot_preview ? p.screenshot_hover_preview : p.keep_expanded_group;
        SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
        InvalidateRect(h, nullptr, FALSE);
    }
}

bool Settings::card_control(int id, int notification) {
    if (selected_ != 1 && selected_ != 2) return false;
    const auto order = selected_ == 1 ? std::span<const int>(card_order) : std::span<const int>(rope_order);
    const auto found = std::find(order.begin(), order.end(), id);
    if (found == order.end()) return false;
    const auto index = static_cast<size_t>(found - order.begin());
    if (index >= rows_.size()) return true;
    auto& row = rows_[index];
    const bool numeric = id == card_font || id == rope_capacity;
    const bool focus = card_choice(id) ? notification == CBN_SETFOCUS :
                       numeric ? notification == EN_SETFOCUS : notification == BN_SETFOCUS;
    if (focus) {
        if (row.top < offset_) scroll(row.top - 8);
        else if (row.top + row.height > offset_ + view_height_) scroll(row.top + row.height - view_height_ + 8);
        return true;
    }
    if (notification != (card_choice(id) ? CBN_SELCHANGE : numeric ? EN_KILLFOCUS : BN_CLICKED)) return true;
    if (saving_.pending() || !IsWindowEnabled(row.value)) return true;
    auto value = data_.cards;
    auto capacity = data_.default_capacity;
    UiText error;
    if (card_choice(id)) {
        const auto selected = SendMessageW(row.value, CB_GETCURSEL, 0, 0);
        if (selected != 0 && selected != 1) { sync_card_control(id); return true; }
        if (id == card_action) value.paste_on_double_click = selected == 0;
        else if (id == card_delete_hanging) value.recycle_hanging = selected == 1;
        else value.recycle_floating = selected == 1;
    }
    else if (id == card_resolution) value.hover_resolution = !value.hover_resolution;
    else if (id == card_thumbnails) value.image_file_previews = !value.image_file_previews;
    else if (id == card_keep_group) value.keep_expanded_group = !value.keep_expanded_group;
    else if (id == card_screenshot_preview) value.screenshot_hover_preview = !value.screenshot_hover_preview;
    else if (id == card_follow_head) value.follow_card_head = !value.follow_card_head;
    else if (id == rope_follow_collector) value.follow_collector = !value.follow_collector;
    else if (id == card_font) {
        if (!number(row.value, value.font_size) || !(value.font_size >= 6 && value.font_size <= 96))
            error = tr(TextKey::error_font_size);
    } else if (!number(row.value, capacity) || capacity < 1) error = tr(TextKey::error_positive_number);
    const auto finish = [this, id, index, value, capacity](UiText error) {
        if (error.empty()) {
            data_.cards = value;
            data_.default_capacity = capacity;
        }
        sync_card_control(id);
        auto& row = rows_[index];
        const auto description = error.empty() ? settings[(selected_ == 1 ? 0 : std::size(card_order)) + index].description : error;
        set_ui_text(row.detail, description);
        accessible_description(row.value, description);
        if (!error.empty()) announce_in_viewport(viewport_, tr(TextKey::settings_short_title), description);
        layout();
    };
    if (!error.empty() || (value == data_.cards && capacity == data_.default_capacity)) finish(error);
    else {
        if (card_choice(id)) sync_card_control(id);
        std::function<void()> write;
        if (id == rope_capacity) write = [save = data_.save_capacity, capacity] { save(capacity); };
        else write = [save = data_.save_cards, value] { save(value); };
        submit_setting(row.detail, std::move(write), [finish](auto error) {
            finish(error ? tr(TextKey::error_setting_save) : UiText{});
        });
    }
    return true;
}
} // namespace cliprope::win
