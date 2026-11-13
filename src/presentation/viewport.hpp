#pragma once

#include "presentation/image_card_layout.hpp"
#include <functional>
#include <optional>
#include <string_view>
namespace cliprope::localization { class LocalizationManager; }

namespace cliprope::presentation {

// Map durable positions to the temporary order shown during a drag.
struct InsertionPreview {
    std::optional<std::size_t> removed, gap;
    float gap_width{}, gap_advance{}, gap_inset{}, removed_advance{};
    std::optional<std::size_t> position(std::size_t index, std::size_t capacity) const {
        if (removed == index)
            return {};
        if (removed && index > *removed)
            --index;
        if (gap && index >= *gap)
            ++index;
        return gap && index >= capacity ? std::nullopt : std::optional(index);
    }
};

struct RopeSlot {
    domain::Rope rope;
    std::size_t index;
    ImageCardLayout geometry;
    std::optional<float> scrolled_origin;
};

struct ViewportReads {
    std::function<std::size_t(std::string_view)> count;
    std::function<std::vector<application::CardSummary>(std::string_view, std::size_t, std::size_t)> page;
    std::function<std::optional<std::size_t>(std::string_view, std::string_view)> position;
    std::function<application::Anchor(std::string_view)> load_anchor;
    std::function<void(std::string_view, const application::Anchor &)> save_anchor;
    std::function<std::size_t()> rope_count;
    std::function<std::vector<domain::Rope>(std::size_t, std::size_t)> ropes;
    std::function<std::optional<std::size_t>(std::string_view)> rope_position;
    std::function<bool(std::string_view)> load_compact;
    std::function<void(std::string_view, bool, const application::Anchor&)> save_compact;
    std::function<std::size_t()> total_rope_count;
    std::function<bool()> rebin_visible;
};

class Viewport {
  public:
    static constexpr float margin = 14, paper_width = 172;
    explicit Viewport(ViewportReads reads);
    void open(std::string rope, float width, std::optional<size_t> target = {});
    void reveal(size_t index);
    void refresh(float width);
    void retranslate(const localization::LocalizationManager& labels);
    void pin_entry(domain::Id id) { pinned_ = std::move(id); }
    void accept_text_edit(const application::CardSummary& saved) {
        for (auto& card : cards_) if (card.entry.id == saved.entry.id) { card.accept_text_edit(saved); break; }
    }
    // A proposed scroll can be checked before writing its durable anchor.
    Viewport preview_navigation() const;
    void scroll(double delta);
    bool can_scroll(double direction) const;
    // The drag owns the final anchor write when the pointer leaves the edge.
    void scroll_for_drag(double delta);
    void align_drop(std::size_t index, float slot_origin);
    void latest();
    void reserve_drag(float width);
    void wheel(double turns, bool reload = false);
    void reload_previews() { if (!rope_.empty()) realize(); }
    ImageCardLayout geometry(std::size_t index) const;
    SizeF work_area{1.e6f, 1.e6f};
    ImageCardLayout measure(const application::CardSummary& card) const {
        return ImageCardLayout::measure(card, paper(), compact_).fit_workspace(work_area);
    }
    float advance(std::size_t index) const;
    float projected_left(std::size_t projected, const InsertionPreview& preview) const;
    void set_compact(bool enabled);
    void resize_card(std::size_t index, domain::CardScale scale, float hook_x);
    float row_height(float image_height) const;
    bool compact() const { return compact_; }
    float step() const { return measure({}).advance(); }
    float paper() const { return compact_ ? 86.f : paper_width; }
    void release();
    std::size_t total() const { return count_; }
    std::size_t first_realized() const { return begin_; }
    const std::vector<application::CardSummary> &cards() const { return cards_; }
    float left(std::size_t position) const;
    std::size_t nearest(float client_x) const;
    std::size_t insertion(float client_x, const InsertionPreview &preview, std::size_t capacity) const;
    std::size_t new_heads() const { return new_heads_; }
    bool at_head() const { return first_ == 0 && offset_ == 0; }
    const application::Anchor &anchor() const { return anchor_; }

  private:
    void realize(bool reuse_page = false, bool moving = false, std::optional<double> phase = {}, bool preserve_anchor = false);
    void remember();
    float origin(std::size_t position) const;
    ViewportReads reads_;
    std::string rope_;
    domain::Id pinned_;
    application::Anchor anchor_;
    std::size_t count_ = 0, begin_ = 0, first_ = 0, new_heads_ = 0;
    double offset_ = 0;
    std::vector<ImageCardLayout> layouts_;
    std::vector<float> lefts_;
    float width_ = 0, drag_buffer_ = 0;
    bool compact_ = false, has_images_ = false;
    std::vector<application::CardSummary> cards_;
};
} // namespace cliprope::presentation
