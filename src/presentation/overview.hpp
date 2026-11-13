#pragma once
#include "presentation/hanging_layout.hpp"
#include "presentation/viewport.hpp"

namespace cliprope::presentation {
struct RopeRow {
    domain::Rope rope;
    Viewport viewport;
};

struct RopeDragLayout {
    std::size_t insertion;
    std::vector<float> tops;
};

class Overview {
  public:
    SizeF work_area{1.e6f, 1.e6f};
    float row_height = image_row_height(domain::ImageSizeMode::standard);
    explicit Overview(ViewportReads reads) : reads_(std::move(reads)) {}
    void open(std::string_view active, float width, float height);
    void refresh(float width, float height);
    void reflow(float width, float height) { realize(width, height, {}, 0, true); }
    void scroll_rows(int delta, float width, float height);
    void reveal(std::string_view id, float width, float height);
    void reveal_card(const domain::Id& rope, size_t card, float width, float height);
    std::optional<std::size_t> position(std::string_view id) const;
    domain::Rope at(std::size_t index) const;
    const domain::Id& head() const { return head_; }
    void preview_move(std::size_t source, std::size_t target, float width, float height);
    RopeDragLayout drag_layout(std::size_t gap, float carried_top, float carried_height) const;
    void clear_move() { move_.reset(); }
    void release() {
        rows_.clear();
        first_ = 0;
        clear_move();
    }
    std::vector<RopeRow> &rows() { return rows_; }
    const std::vector<RopeRow> &rows() const { return rows_; }
    std::size_t first() const { return first_; }

  private:
    void realize(float width, float height, std::string_view target = {}, size_t card = 0, bool reuse = false);
    ViewportReads reads_;
    domain::Id head_;
    std::size_t first_{};
    std::optional<std::pair<std::size_t, std::size_t>> move_;
    std::vector<RopeRow> rows_;
};
} // namespace cliprope::presentation
