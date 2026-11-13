#include "platform_win/settings.hpp"
#include "platform_win/fluent_list.hpp"
#include <algorithm>

namespace cliprope::win {
using namespace fluent;
namespace {
const UiText labels[]{tr(TextKey::settings_language), tr(TextKey::settings_topmost), tr(TextKey::settings_launch), tr(TextKey::settings_reset)};
const UiText descriptions[]{nullptr, tr(TextKey::settings_topmost_description), tr(TextKey::settings_launch_description), nullptr};
constexpr localization::Language languages[]{localization::Language::automatic, localization::Language::english,
                                             localization::Language::simplified_chinese};
const UiText language_names[]{tr(TextKey::settings_language_auto), tr(TextKey::settings_language_english),
                              tr(TextKey::settings_language_chinese)};
}

void Settings::general_rows() {
    for (size_t index = 0; index < std::size(general_order); ++index) {
        const auto id = general_order[index];
        Row r{};
        if (id == language) heading(r, tr(TextKey::settings_application));
        if (id == launch) heading(r, tr(TextKey::settings_startup));
        if (id == reset) heading(r, tr(TextKey::settings_advanced));
        if (id != reset) r.label = child(viewport_, L"STATIC", labels[index], SS_NOPREFIX, 0);
        r.detail = child(viewport_, L"STATIC", descriptions[index], SS_NOPREFIX | SS_EDITCONTROL, 0);
        if (r.label) surface_.font(r.label);
        surface_.font(r.detail, Font::detail);
        if (id == language) {
            r.value = choice_box(surface_, viewport_, labels[index], id);
            if (!r.value) throw std::runtime_error("Could not create the language choice.");
        } else if (id == reset) {
            const auto action = tr(TextKey::settings_reset_action);
            r.value = child(viewport_, L"BUTTON", action, WS_TABSTOP | BS_NOTIFY, id);
            surface_.card_style(r.value, {.part = CardPart::dialog_button, .label = action});
        } else {
            r.value = child(viewport_, L"BUTTON", labels[index], WS_TABSTOP | BS_CHECKBOX | BS_NOTIFY, id);
            surface_.card_style(r.value, {CardPart::settings_switch});
        }
        surface_.font(r.value);
        accessible_name(r.value, labels[index]);
        accessible_description(r.value, descriptions[index]);
        rows_.push_back(r);
    }
    sync_general();
}
void Settings::sync_language_choice(bool relabel) {
    const auto choice = GetDlgItem(viewport_, language);
    if (!choice) return;
    if (relabel || SendMessageW(choice, CB_GETCOUNT, 0, 0) != std::size(languages)) {
        SendMessageW(choice, CB_RESETCONTENT, 0, 0);
        for (size_t i = 0; i < std::size(languages); ++i) {
            const auto index = SendMessageW(choice, CB_ADDSTRING, 0, LPARAM(language_names[i].c_str()));
            SendMessageW(choice, CB_SETITEMDATA, index, static_cast<LPARAM>(languages[i]));
        }
    }
    const auto found = std::find(std::begin(languages), std::end(languages), data_.language);
    const auto index = found == std::end(languages) ? 0 : found - std::begin(languages);
    SendMessageW(choice, CB_SETCURSEL, index, 0);
    surface_.card_style(choice, {.part = CardPart::settings_choice, .label = language_names[index]});
}

void Settings::sync_general() {
    UiText error;
    try {
        if (data_.refresh_settings) data_.refresh_settings(data_);
    } catch (const std::exception&) {
        error = tr(TextKey::error_setting_save);
    }
    sync_language_choice(false);
    for (size_t index = 0; index < std::size(general_order); ++index) {
        const auto id = general_order[index];
        auto& r = rows_[index];
        if (id == launch || id == topmost) {
            const bool on = id == launch ? data_.launch_at_sign_in.value_or(false) : data_.topmost;
            SendMessageW(r.value, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
            InvalidateRect(r.value, nullptr, FALSE);
        }
        bool enabled{};
        switch (id) {
        case language: enabled = bool(data_.save_language); break;
        case launch: enabled = data_.save_startup && data_.launch_at_sign_in.has_value(); break;
        case topmost: enabled = bool(data_.save_topmost); break;
        case reset: enabled = bool(data_.reset_settings); break;
        }
        EnableWindow(r.value, enabled && error.empty());
        const UiText detail = !error.empty() ? error : id == launch && !data_.startup_notice.empty()
                                ? data_.startup_notice : descriptions[index];
        set_ui_text(r.detail, detail);
        accessible_description(r.value, detail);
    }
}

bool Settings::general_control(int id, int notification) {
    if (selected_ != 0 || id < language || id > reset) return false;
    if (rows_.size() != std::size(labels)) return true;
    auto& row = rows_[std::find(std::begin(general_order), std::end(general_order), id) - std::begin(general_order)];
    if (notification == (id == language ? CBN_SETFOCUS : BN_SETFOCUS)) {
        if (row.top < offset_) scroll(row.top - 8);
        else if (row.top + row.height > offset_ + view_height_) scroll(row.top + row.height - view_height_ + 8);
    }
    if (notification != (id == language ? CBN_SELCHANGE : BN_CLICKED) || !IsWindowEnabled(row.value)) return true;
    std::function<void()> write;
    auto chosen = data_.language;
    if (id == language) {
        const auto selected = SendMessageW(row.value, CB_GETCURSEL, 0, 0);
        if (selected < 0 || selected >= std::size(languages)) return true;
        chosen = languages[selected];
        write = [save = data_.save_language, chosen] { save(chosen); };
        sync_language_choice(false);
    } else if (id == reset) {
        const auto confirmation = tr(data_.save_startup ? TextKey::settings_reset_with_startup : TextKey::settings_reset_isolated);
        if (!management_prompt(window_, tr(TextKey::settings_reset_confirm), tr(TextKey::settings_global), confirmation,
                               tr(TextKey::settings_reset), true, 275)) return true;
        write = data_.reset_settings;
    } else if (id == launch)
        write = [save = data_.save_startup, value = !data_.launch_at_sign_in.value()] { save(value); };
    else write = [save = data_.save_topmost, value = !data_.topmost] { save(value); };
    submit_setting(row.detail, std::move(write), [this, id, chosen](auto error) {
        if (!error && id == language) data_.language = chosen;
        sync_general();
        const auto detail = error ? tr(TextKey::error_setting_save) : id == reset ? tr(TextKey::settings_reset_done) : UiText{};
        if (!detail.empty()) {
            auto& r = rows_[std::find(std::begin(general_order), std::end(general_order), id) - std::begin(general_order)];
            set_ui_text(r.detail, detail);
            accessible_description(r.value, detail);
            announce_in_viewport(viewport_, tr(TextKey::settings_short_title), detail);
        }
        layout();
    });
    return true;
}
} // namespace cliprope::win
