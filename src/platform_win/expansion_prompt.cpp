#include "platform_win/expansion_prompt.hpp"
#include "platform_win/fluent.hpp"

namespace cliprope::win {
bool review_file_group_expansion(HWND owner, const std::wstring& rope_name, std::size_t member_count,
                                 const application::ExpansionImpact& impact, bool keep_source) {
    const auto removed = impact.removed_tail.size();
    const bool confirm = !impact.required_capacity && removed != 0;
    const auto key = confirm ? keep_source ? removed == 1 ? TextKey::card_expand_keep_one : TextKey::card_expand_keep_other
                                          : removed == 1 ? TextKey::card_expand_replace_one : TextKey::card_expand_replace_other
                             : keep_source ? TextKey::card_expand_keep_capacity : TextKey::card_expand_replace_capacity;
    const auto description = confirm
        ? message(key, {{"members", std::to_wstring(member_count)}, {"removed", std::to_wstring(removed)}})
        : message(key, {{"members", std::to_wstring(member_count)}, {"capacity", std::to_wstring(impact.required_capacity)}});
    return fluent::management_prompt(owner, confirm ? tr(TextKey::card_expand_confirm) : tr(TextKey::card_expand_capacity_title),
                                     rope_name, description, confirm ? tr(TextKey::card_expand_action) : tr(TextKey::common_ok), confirm, 198);
}
} // namespace cliprope::win
