#pragma once
#include "application/browse.hpp"
#include "localization/localization.hpp"
#include <array>

namespace cliprope::presentation {
inline const localization::LocalizationManager& english_labels() {
    static const localization::LocalizationManager english;
    return english;
}
inline void localize_card(application::CardSummary& card, const localization::LocalizationManager& labels) {
    using localization::TextKey;
    if (card.formats.empty() && !card.generated_text && card.kind != application::CardKind::unavailable) return;
    static constexpr std::array names{TextKey::card_text, TextKey::card_format_html, TextKey::card_format_rtf,
        TextKey::card_format_image, TextKey::card_format_files, TextKey::card_format_virtual};
    static_assert(names.size() == static_cast<size_t>(application::CardFormat::count));
    std::wstring text;
    for (auto format : card.formats) {
        if (!text.empty()) text += L" · ";
        text += labels.tr(names.at(static_cast<size_t>(format)));
    }
    if (card.status == application::CardStatus::damaged || text.empty()) text = labels.tr(TextKey::card_unavailable);
    else if (card.status == application::CardStatus::partial) text += L" · " + std::wstring(labels.tr(TextKey::card_partial));
    using Kind = application::CardSummary::FilePreviewKind;
    if (card.status != application::CardStatus::damaged && card.file_preview != Kind::none)
        text = labels.tr(card.file_preview == Kind::picture ? TextKey::card_image_reference : TextKey::card_image_group);
    if (card.availability == domain::FileAvailability::missing) text += L" · " + std::wstring(labels.tr(TextKey::card_original_missing));
    if (card.availability == domain::FileAvailability::inaccessible) text += L" · " + std::wstring(labels.tr(TextKey::card_original_inaccessible));
    card.capabilities = std::move(text);
    if (card.generated_text) card.text = card.capabilities;
    else if (card.empty_text) card.text = labels.tr(TextKey::card_empty_text);
}
inline application::CardSummary english_card(application::CardSummary card) {
    localize_card(card, english_labels());
    return card;
}
inline std::wstring detail_label(application::DetailText code, std::wstring_view raw,
                                  const localization::LocalizationManager& labels) {
    using application::DetailText;
    using localization::TextKey;
    const auto text = [&](TextKey key) { return std::wstring(labels.tr(key)); };
    switch (code) {
    case DetailText::capture_details: return text(TextKey::card_info_capture_details);
    case DetailText::saved: return text(TextKey::card_info_saved);
    case DetailText::source: return text(TextKey::card_info_source);
    case DetailText::capture: return text(TextKey::card_info_capture);
    case DetailText::file_health: return text(TextKey::card_info_file_health);
    case DetailText::original_path: return text(TextKey::card_info_original_path);
    case DetailText::file_status: return text(TextKey::card_info_file_status);
    case DetailText::preview: return text(TextKey::card_info_preview);
    case DetailText::inventory: return text(TextKey::card_info_inventory);
    case DetailText::partial_inventory: return text(TextKey::card_info_partial_inventory);
    case DetailText::incomplete_inventory: return text(TextKey::card_info_incomplete_inventory);
    case DetailText::unknown: return text(TextKey::card_info_unknown);
    case DetailText::missing: return text(TextKey::card_info_missing);
    case DetailText::inaccessible: return text(TextKey::card_info_inaccessible);
    case DetailText::refresh: return text(TextKey::card_info_refresh);
    case DetailText::unchecked: return text(TextKey::card_info_unchecked);
    case DetailText::available: return text(TextKey::card_info_available);
    case DetailText::not_applicable: return text(TextKey::card_info_not_applicable);
    case DetailText::decode_failed: return text(TextKey::card_info_decode_failed);
    case DetailText::ready: return text(TextKey::card_info_ready);
    case DetailText::cached: return text(TextKey::card_info_cached);
    case DetailText::unavailable: return text(TextKey::hotkey_check_unavailable);
    case DetailText::card_gone: return text(TextKey::error_card_gone);
    case DetailText::information_unavailable: return text(TextKey::error_card_information);
    case DetailText::path: return text(TextKey::card_path);
    case DetailText::local_time: return labels.format(TextKey::card_info_local_time, {{"timestamp", std::wstring(raw)}});
    case DetailText::windows_format: return labels.format(TextKey::card_info_windows_format, {{"number", std::wstring(raw)}});
    case DetailText::capture_status:
        if (raw == L"captured") return text(TextKey::card_info_captured);
        if (raw == L"complete") return text(TextKey::card_info_complete);
        if (raw == L"partial") return text(TextKey::card_partial);
        if (raw == L"failed") return text(TextKey::card_info_failed);
        break;
    case DetailText::format_status:
        if (raw == L"saved") return text(TextKey::card_info_status_saved);
        if (raw == L"skipped") return text(TextKey::card_info_status_skipped);
        if (raw == L"failed") return text(TextKey::card_info_failed);
        if (raw == L"damaged or missing") return text(TextKey::card_info_damaged_missing);
        break;
    default: break;
    }
    return std::wstring(raw);
}
inline std::pair<std::wstring, std::wstring> detail_labels(const application::DetailLine& line,
                                                         const localization::LocalizationManager& labels) {
    auto name = detail_label(line.label_text, line.label, labels);
    if (line.stream_index >= 0) name += L" [" + std::to_wstring(line.stream_index) + L"]";
    return {std::move(name), detail_label(line.value_text, line.value, labels)};
}
} // namespace cliprope::presentation
