#pragma once

#include "application/browse.hpp"
#include "presentation/card_content.hpp"
#include <array>
#include <string_view>

namespace cliprope::presentation {

using CardFormat = application::CardFormat;

// Inputs describe verified adapter outcomes. Labels keep the capture order,
// even when multiple representations provide the same visible capability.
class CardCapabilities {
public:
    explicit CardCapabilities(bool partial_capture) : partial(partial_capture) {}

    void retain(CardFormat type) {
        const auto i = static_cast<std::size_t>(type);
        auto& retained = seen_.at(i);
        if (retained) return;
        retained = true;
        types_.push_back(type);
    }

    void apply(application::CardSummary& card) const {
        using application::CardKind;
        const auto has = [&](CardFormat f) { return seen_[static_cast<std::size_t>(f)]; };
        const bool has_text = has(CardFormat::text);
        card.formats = types_;
        card.generated_text = !has_text || card.text_unavailable;
        card.empty_text = has_text && !card.text_unavailable && card.text.empty();
        card.image_primary = !has_text && has(CardFormat::image);
        card.status = damaged || card.text_unavailable ? application::CardStatus::damaged
                    : partial ? application::CardStatus::partial
                              : application::CardStatus::complete;
        if (types_.empty()) card.kind = CardKind::unavailable;
        else if (has_text) card.kind = is_link(card.text) ? CardKind::link : CardKind::text;
        else if (has(CardFormat::image)) card.kind = CardKind::image;
        else if (has(CardFormat::file_references) || has(CardFormat::virtual_files)) card.kind = CardKind::files;
        else card.kind = CardKind::rich;
    }

    bool partial;
    bool damaged = false;

private:
    std::vector<CardFormat> types_;
    std::array<bool, static_cast<std::size_t>(CardFormat::count)> seen_{};
};
} // namespace cliprope::presentation
