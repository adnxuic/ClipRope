#pragma once

#include "application/browse.hpp"
#include "presentation/hanging_layout.hpp"
#include <algorithm>
#include <cmath>

namespace cliprope::presentation {
struct SizeF { float width, height; };

inline SizeF image_limit(domain::ImageSizeMode mode) {
    switch (mode) {
    case domain::ImageSizeMode::compact: return {144, 108};
    case domain::ImageSizeMode::large: return {256, 192};
    default: return {192, 144};
    }
}

inline bool adaptive_image(const application::CardSummary& card) {
    return card.image_primary || card.kind == application::CardKind::image ||
           card.file_preview == application::CardSummary::FilePreviewKind::picture;
}

// Horizontal coordinates are relative to the hook. Vertical coordinates are
// relative to the rope centerline. All measurements are in logical DIPs.
struct ImageCardLayout {
    float width, body_width, top, bottom, bar_bottom;
    bool adaptive;
    float height() const { return bottom - top; }
    float hanger_width() const { return adaptive ? width : width + 2 * Hanging::bar_overhang; }
    static constexpr float gap = 24, shadow_margin = 2;
    float spacing = gap;
    float inset() const { return (hanger_width() - width) / 2 + shadow_margin; }
    float advance() const { return hanger_width() + 2 * shadow_margin + spacing; }
    bool operator==(const ImageCardLayout&) const = default;
    ImageCardLayout fit_workspace(SizeF available) const {
        auto result = *this;
        const float w = std::max(1.f, available.width - 2 * (Hanging::bar_overhang + shadow_margin));
        const float h = std::max(1.f, available.height - Hanging::rope_end - Hanging::max_sag - top - 24);
        if (adaptive) {
            const float fit = std::min({1.f, w / body_width, h / height()});
            result.body_width *= fit;
            result.bottom = top + height() * fit;
            const float density = spacing / gap;
            result.width = std::min(w, std::max(160 * density, result.body_width + 12 * density));
        } else {
            result.width = result.body_width = std::min(width, w);
            result.bottom = top + std::min(height(), h);
        }
        return result;
    }
    float carried_padding() const { return std::max(32.f, std::ceil(bottom * 0.08f + 12)); }
    float carried_height() const { return std::max(240.f, bottom + 24 + width * 0.04f + 16); }
    float float_width() const { return std::max(144.f, body_width); }
    static constexpr float toolbar = 38;
    SizeF floating_size(domain::CardScale scale) const {
        return adaptive ? SizeF{float_width(), toolbar + height()} : SizeF{240 * scale.width, 160 * scale.height};
    }
    SizeF fitted_image(float window_width, float window_height) const {
        const float fit = std::clamp(std::min(window_width / body_width, (window_height - toolbar) / height()), 0.f, 1.f);
        return {body_width * fit, height() * fit};
    }

    static ImageCardLayout measure(const application::CardSummary& card, float paper = 172, bool compact = false) {
        const float spacing = compact ? 12.f : gap;
        if (!adaptive_image(card)) {
            const float width = paper * card.scale.width;
            const float height = (Hanging::paper_bottom - Hanging::paper_top) * card.scale.height;
            return {width, width, Hanging::paper_top, Hanging::paper_top + height, Hanging::bar_bottom, false, spacing};
        }
        const auto limit = image_limit(card.image_preferences.size);
        float w = float(card.source.width), h = float(card.source.height);
        if (!(w > 0 && h > 0)) {
            // Legacy offline thumbnails may be the only surviving shape hint.
            w = card.thumbnail && card.thumbnail->width ? float(card.thumbnail->width) : 180.f;
            h = card.thumbnail && card.thumbnail->height ? float(card.thumbnail->height) : 120.f;
        }
        const float scale = std::min({limit.width / w, limit.height / h, 1.5f});
        w *= scale * card.scale.width;
        h *= scale * card.scale.width;
        const float density = compact ? 0.5f : 1.f;
        constexpr float bar_end = Hanging::bar_top + 30, top = bar_end - 2;
        return {std::max(160.f, w + 12) * density, w * density, top, top + h * density, bar_end, true, spacing};
    }
};

inline float image_row_height(domain::ImageSizeMode mode) {
    const float image_row = Hanging::rope_end + Hanging::max_sag + Hanging::bar_top + 30 +
                            image_limit(mode).height + 32;
    return std::max(Hanging::row, image_row);
}
} // namespace cliprope::presentation
