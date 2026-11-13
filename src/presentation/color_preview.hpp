#pragma once
#include "application/browse.hpp"

namespace cliprope::presentation {
// UI-thread-only render overrides. They never enter the persisted read model.
struct CardColorPreview {
    domain::Id entry;
    bool paper{};
    domain::ColorSelection color;
};
struct RopeColorPreview {
    domain::Id rope;
    domain::RopeMaterial material;
};
struct ColorPreview {
    std::optional<CardColorPreview> card;
    std::optional<RopeColorPreview> rope;
};
inline application::CardStyle preview_style(const ColorPreview& preview, std::string_view id, application::CardStyle style) {
    if (preview.card && preview.card->entry == id)
        (preview.card->paper ? style.background_color : style.text_color) = preview.card->color;
    return style;
}
class PreviewCard {
    std::optional<application::CardSummary> draft_;
    const application::CardSummary &source_;

  public:
    PreviewCard(const application::CardSummary &source, const ColorPreview* preview) : source_(source) {
        if (preview && preview->card && preview->card->entry == source.entry.id) {
            draft_ = source;
            draft_->style = preview_style(*preview, source.entry.id, source.style);
        }
    }
    const application::CardSummary &get() const { return draft_ ? *draft_ : source_; }
};
} // namespace cliprope::presentation
