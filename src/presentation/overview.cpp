#include "presentation/overview.hpp"
#include <algorithm>
#include <stdexcept>

namespace cliprope::presentation {
std::optional<std::size_t> Overview::position(std::string_view id) const {
    const auto count = reads_.rope_count();
    const auto pos = reads_.rope_position(id), head = reads_.rope_position(head_);
    if (!count || !pos || !head) return {};
    return (*pos + count - *head) % count;
}
domain::Rope Overview::at(std::size_t index) const {
    const auto count = reads_.rope_count();
    const auto head = reads_.rope_position(head_);
    if (!head || index >= count) throw std::out_of_range("Overview row unavailable");
    auto page = reads_.ropes((*head + index) % count, 1);
    if (page.empty()) throw std::out_of_range("Overview row unavailable");
    return std::move(page.front());
}
void Overview::realize(float width, float height, std::string_view target, size_t card, bool reuse) {
    const auto count = reads_.rope_count();
    first_ = std::min(first_, count ? count - 1 : 0);
    std::vector<RopeRow> next;
    auto read = [&](std::size_t index) {
        if (move_) {
            const auto [from, to] = *move_;
            if (index == to) index = from;
            else if (from <= index && index < to) ++index;
            else if (to < index && index <= from) --index;
        }
        auto rope = at(index);
        auto existing = std::find_if(rows_.begin(), rows_.end(),
            [&](const auto &row) { return row.rope.id == rope.id; });
        Viewport viewport(reads_);
        viewport.work_area = work_area;
        if (existing != rows_.end()) {
            viewport = std::move(existing->viewport);
            viewport.work_area = work_area;
            if (rope.id==target) viewport.reveal(card);
            else if (!reuse) viewport.refresh(width);
        } else viewport.open(rope.id, width, rope.id==target ? std::optional(card) : std::nullopt);
        return RopeRow{std::move(rope), std::move(viewport)};
    };
    float used = 0;
    for (auto i = first_; i < count && (next.empty() || used + Hanging::rope_end + Hanging::max_sag < height); ++i) {
        next.push_back(read(i));
        used += next.back().viewport.row_height(row_height);
    }
    // At the end, fill from above only if the previous complete row fits.
    while (!reuse && first_ && first_ + next.size() == count && used + std::min(row_height, Hanging::row) <= height) {
        auto row = read(first_ - 1);
        const float previous = row.viewport.row_height(row_height);
        if (used + previous > height) break;
        --first_;
        used += previous;
        next.insert(next.begin(), std::move(row));
    }
    rows_ = std::move(next);
}
void Overview::open(std::string_view active, float width, float height) {
    // Reuse visible viewports by identity when only the circular origin changes.
    // Their density and live horizontal browsing state belong to the rope.
    clear_move();
    first_ = 0;
    head_ = active;
    realize(width, height);
}
void Overview::refresh(float width, float height) {
    if (!move_ && !rows_.empty())
        first_ = position(rows_.front().rope.id).value_or(first_);
    realize(width, height);
}
void Overview::scroll_rows(int delta, float width, float height) {
    if (delta > 0 && first_ + rows_.size() == reads_.rope_count()) {
        float used = 0;
        for (const auto& row : rows_) used += row.viewport.row_height(row_height);
        if (used <= height) return;
    }
    const auto index = static_cast<std::int64_t>(first_) + delta;
    first_ = static_cast<std::size_t>(std::max<std::int64_t>(0, index));
    realize(width, height);
}
void Overview::preview_move(std::size_t source, std::size_t target, float width, float height) {
    move_ = std::pair{source, target};
    realize(width, height);
}
RopeDragLayout Overview::drag_layout(std::size_t gap, float carried_top, float carried_height) const {
    // Remove the projected slot first. The remaining rows have stable centers
    // even when the pointer crosses a center and the slot changes sides.
    const bool gap_above = gap < first_;
    float compact_top = gap_above ? -carried_height : 0.f;
    RopeDragLayout result{first_ - (gap_above ? 1u : 0u), {}};
    result.tops.reserve(rows_.size());
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (first_ + i == gap) {
            result.tops.push_back(carried_top);
            continue;
        }
        const float h = rows_[i].viewport.row_height(row_height);
        const float progress = std::clamp((carried_top - compact_top) / h, 0.f, 1.f);
        result.tops.push_back(compact_top + carried_height * (1.f - progress));
        if (progress >= .5f) ++result.insertion;
        compact_top += h;
    }
    return result;
}
void Overview::reveal_card(const domain::Id& rope, size_t card, float width, float height) {
    float top=0;
    for (auto& row : rows_) {
        const float bottom=top+row.viewport.row_height(row_height);
        if (row.rope.id==rope && (top==0 || bottom<=height)) { row.viewport.reveal(card); return; }
        top=bottom;
    }
    if (auto index=position(rope)) {
        first_=*index;
        realize(width,height,rope,card);
    }
}
void Overview::reveal(std::string_view id, float width, float height) {
    const auto index = position(id);
    if (index && (*index < first_ || *index >= first_ + rows_.size()))
        first_ = *index;
    realize(width, height);
}
} // namespace cliprope::presentation
