#include "presentation/viewport.hpp"
#include "presentation/card_labels.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace cliprope::presentation {
void Viewport::retranslate(const localization::LocalizationManager& labels) {
    for (auto& card : cards_) localize_card(card, labels);
}
Viewport::Viewport(ViewportReads reads) : reads_(std::move(reads)) {}

void Viewport::open(std::string rope, float width, std::optional<size_t> target) {
    release();
    rope_ = std::move(rope);
    compact_ = reads_.load_compact ? reads_.load_compact(rope_) : false;
    width_ = width;
    count_ = reads_.count(rope_);
    if (target) { first_=*target; realize(); return; }
    anchor_ = reads_.load_anchor(rope_);
    if (!anchor_.entry.empty() && std::isfinite(anchor_.offset)) {
        if (auto position = reads_.position(rope_, anchor_.entry)) {
            first_ = *position;
            offset_ = anchor_.offset;
        }
    }
    realize(false, false, {}, true);
}

void Viewport::refresh(float width) {
    if (rope_.empty()) return;
    width_ = width;
    count_ = reads_.count(rope_);
    const auto previous = anchor_;
    // Keep the edited sheet under the caret when capture inserts at the head or a
    // neighboring preview changes width. Explicit scrolling still moves the row.
    if (!pinned_.empty()) {
        const auto old = std::find_if(cards_.begin(), cards_.end(), [&](const auto& c) { return c.entry.id == pinned_; });
        if (old != cards_.end()) {
            if (auto pos = reads_.position(rope_, pinned_)) {
                offset_ = margin - origin(begin_ + static_cast<size_t>(old - cards_.begin()));
                first_ = *pos;
                realize(false, true, {}, true);
                if (previous.entry != anchor_.entry || previous.offset != anchor_.offset) remember();
                return;
            }
        }
    }
    if (!anchor_.entry.empty()) {
        auto pos = reads_.position(rope_, anchor_.entry);
        if (pos && *pos > first_) new_heads_ += *pos - first_;
        for (auto i = first_ - begin_ + 1; !pos && i < cards_.size(); ++i)
            pos = reads_.position(rope_, cards_[i].entry.id);
        if (pos) first_ = *pos;
    }
    realize(false, false, {}, true);
    if (anchor_.entry != previous.entry || anchor_.offset != previous.offset) remember();
}

void Viewport::release() {
    rope_.clear();
    pinned_.clear();
    has_images_ = false;
    drag_buffer_ = 0;
    cards_.clear(); layouts_.clear(); lefts_.clear();
    count_ = begin_ = first_ = new_heads_ = 0;
    offset_ = 0;
    anchor_ = {};
}

void Viewport::realize(bool reuse, bool moving, std::optional<double> phase, bool preserve_anchor) {
    // Build outward from the durable anchor. No read of the prefix of a distant
    // rope is necessary, including on the first open after a restart.
    struct Item { application::CardSummary card; ImageCardLayout layout; };
    std::map<std::size_t, Item> page;
    if (reuse)
        for (std::size_t i = 0; i < cards_.size(); ++i)
            page.emplace(begin_ + i, Item{std::move(cards_[i]), layouts_[i]});
    const auto available = std::max(1.f, width_ - 2 * margin);
    auto fetch = [&](std::size_t i, std::size_t n = 1) -> const Item& {
        if (!page.contains(i)) {
            n = std::min(n, count_ - i);
            if (auto next = page.lower_bound(i); next != page.end()) n = std::min(n, next->first - i);
            auto added = reads_.page(rope_, i, n);
            for (std::size_t j = 0; j < added.size(); ++j) {
                const auto g = measure(added[j]);
                page.emplace(i + j, Item{std::move(added[j]), g});
            }
        }
        return page.at(i);
    };
    auto layout = [&](std::size_t i) { return fetch(i).layout; };
    auto stride = [&](std::size_t i) {
        auto g = layout(i);
        return g.advance();
    };
    cards_.clear(); layouts_.clear(); lefts_.clear();
    anchor_ = {};
    if (!count_) { first_ = begin_ = 0; offset_ = 0; new_heads_ = 0; return; }
    first_ = std::min(first_, count_ - 1);
    if (phase) offset_ = *phase * stride(first_);
    if (!moving) offset_ = std::clamp(offset_, preserve_anchor && !first_ ? -double(width_) : 0., double(stride(first_)) - 0.001);
    while (offset_ < 0 && first_) offset_ += stride(--first_);
    if (!preserve_anchor) offset_ = std::max(offset_, 0.);
    while (first_ + 1 < count_ && offset_ >= stride(first_)) offset_ -= stride(first_++);
    double edge = -offset_;
    auto end = first_;
    while (end < count_ && edge < available) {
        fetch(end, std::size_t(std::clamp((available - edge) / 500., 1., 256.)));
        edge += stride(end++);
    }
    // Clamp the trailing edge, backing into the preceding page only as needed.
    if (end == count_ && !preserve_anchor) {
        edge -= layout(end - 1).spacing;
        if (edge < available) {
            offset_ -= available - edge;
            while (offset_ < 0 && first_) offset_ += stride(--first_);
            offset_ = std::max(0., offset_);
        }
    }
    begin_ = first_ ? first_ - 1 : 0;
    end = std::min(count_, end + 1);
    // Projection can shift several narrow neighbors into view when a wide
    // source leaves the row, or expose a preceding card before an inserted gap.
    float left_buffer = first_ > begin_ ? stride(begin_) : 0;
    while (begin_ && left_buffer < drag_buffer_) left_buffer += stride(--begin_);
    float right_buffer = end > first_ ? stride(end - 1) : 0;
    while (end < count_ && right_buffer < drag_buffer_) right_buffer += stride(end++);
    float x = margin - float(offset_) - left_buffer;
    for (std::size_t i = begin_; i < end; ++i) {
        layouts_.push_back(layout(i));
        has_images_ |= layouts_.back().adaptive;
        lefts_.push_back(x);
        x += stride(i);
        cards_.push_back(std::move(page.at(i).card));
    }
    if (!at_head()) anchor_ = {cards_[first_ - begin_].entry.id, offset_};
    else new_heads_ = 0;
}

float Viewport::row_height(float image_height) const {
    float height = has_images_ ? image_height : Hanging::row;
    for (const auto& g : layouts_) {
        // The normal row already includes clearance below an upright paper.
        height = std::max(height, Hanging::row + g.bottom - Hanging::paper_bottom);
    }
    return height;
}

void Viewport::resize_card(std::size_t index, domain::CardScale scale, float hook_x) {
    auto& card = cards_.at(index - begin_);
    card.scale = scale;
    auto& layout = layouts_.at(index - begin_);
    const float previous = layout.advance();
    layout = measure(card);
    const float center = left(index) + layout.width / 2;
    // Slots before the anchor are laid out backwards from it.
    offset_ += center - hook_x - (index < first_ ? layout.advance() - previous : 0.f);
    realize(true, true, {}, true);
}

void Viewport::reserve_drag(float width) {
    if (drag_buffer_ == width) return;
    drag_buffer_ = width;
    if (!rope_.empty()) realize(true);
}

Viewport Viewport::preview_navigation() const {
    auto next = *this;
    next.reads_.save_anchor = [](auto, const auto&) {};
    return next;
}

void Viewport::remember() { reads_.save_anchor(rope_, anchor_); }
bool Viewport::can_scroll(double direction) const {
    if (cards_.empty()) return false;
    if (direction < 0) return first_ > 0 || offset_ > 0;
    if (direction <= 0) return false;
    if (begin_ + cards_.size() < count_) return true;
    const auto& last = layouts_.back();
    return lefts_.back() + last.advance() - last.spacing > width_ - margin + 0.001f;
}
void Viewport::scroll_for_drag(double delta) {
    if (!count_ || !std::isfinite(delta)) return;
    offset_ += delta;
    realize(true, true);
}
void Viewport::align_drop(std::size_t index, float slot_origin) {
    if (index >= count_ || !std::isfinite(slot_origin)) return;
    if (index >= begin_ && index - begin_ < cards_.size() && std::abs(origin(index) - slot_origin) < 0.01f) return;
    // A reorder can move the old anchor to a different index during the commit.
    // Place the committed slot at the gap, subject to the normal end clamps.
    first_ = index;
    offset_ = margin - slot_origin;
    realize(true, true);
    remember();
}
void Viewport::scroll(double delta) {
    if (rope_.empty() || !std::isfinite(delta)) return;
    scroll_for_drag(delta);
    remember();
}
void Viewport::wheel(double turns, bool reload) {
    if (!std::isfinite(turns) || !count_) return;
    const double target = std::clamp(double(first_) + offset_ / advance(first_) + turns,
                                     0., double(count_));
    first_ = std::min(std::size_t(target), count_ - 1);
    realize(!reload, false, target - first_);
    remember();
}
void Viewport::reveal(size_t index) {
    if (index>=count_) return;
    if (index>=begin_ && index-begin_<cards_.size() &&
        left(index)>=0 && left(index)+geometry(index).width<=width_) return;
    first_=index; offset_=0;
    realize();
}
void Viewport::latest() {
    if (rope_.empty()) return;
    first_ = 0; offset_ = 0; realize(); remember();
}
void Viewport::set_compact(bool enabled) {
    if (compact_ == enabled) return;
    // Prepare the complete layout before saving. A rejected write leaves the
    // live viewport and its durable density/anchor unchanged.
    auto next = *this;
    next.compact_ = enabled;
    if (!rope_.empty()) {
        const double phase = count_ ? offset_ / advance(first_) : 0.;
        next.realize(false, false, phase, true);
        if (reads_.save_compact) reads_.save_compact(rope_, enabled, next.anchor_);
        else reads_.save_anchor(rope_, next.anchor_);
    }
    *this = std::move(next);
}
ImageCardLayout Viewport::geometry(std::size_t i) const {
    if (i >= begin_ && i - begin_ < layouts_.size()) return layouts_[i - begin_];
    return measure({});
}
float Viewport::advance(std::size_t i) const {
    const auto g = geometry(i);
    return g.advance();
}
float Viewport::origin(std::size_t i) const {
    if (lefts_.empty()) return margin;
    if (i < begin_) return lefts_.front() - float(begin_ - i) * step();
    if (i - begin_ < lefts_.size()) return lefts_[i - begin_];
    const auto last = begin_ + lefts_.size() - 1;
    return lefts_.back() + advance(last) + float(i - last - 1) * step();
}
float Viewport::left(std::size_t i) const { return origin(i) + geometry(i).inset(); }
float Viewport::projected_left(std::size_t pos, const InsertionPreview& p) const {
    // A gap is inserted into the list after the moving source is removed.
    const bool after_gap = p.gap && pos > *p.gap;
    auto original = pos - (after_gap ? 1 : 0);
    const bool after_source = p.removed && original >= *p.removed;
    if (after_source) ++original;
    const auto inset = p.gap == pos && p.gap_width > 0 ? p.gap_inset : geometry(original).inset();
    const float removed = after_source ? (p.removed_advance > 0 ? p.removed_advance : advance(*p.removed)) : 0;
    return origin(original) + inset - removed +
           (after_gap ? (p.gap_advance > 0 ? p.gap_advance : step()) : 0);
}
std::size_t Viewport::nearest(float x) const {
    for (auto i = begin_; i + 1 < begin_ + cards_.size(); ++i) {
        const float mid = (left(i) + geometry(i).width / 2 + left(i+1) + geometry(i+1).width / 2) / 2;
        if (x < mid) return i;
    }
    return cards_.empty() ? 0 : begin_ + cards_.size() - 1;
}
std::size_t Viewport::insertion(float x, const InsertionPreview& p, std::size_t capacity) const {
    const auto count = total() - (p.removed && *p.removed < total() ? 1 : 0);
    const auto limit = std::min(count, capacity ? capacity - 1 : 0);
    auto last = std::min(begin_, limit);
    for (auto i = begin_; i < begin_ + cards_.size(); ++i) {
        if (p.removed == i) continue;
        const auto pos = i - (p.removed && *p.removed < i ? 1 : 0);
        last = std::min(pos + 1, limit);
        const auto shown = pos + (p.gap && pos >= *p.gap ? 1 : 0);
        if (x <= projected_left(shown, p) + geometry(i).width / 2) return std::min(pos, limit);
    }
    return last;
}
} // namespace cliprope::presentation
