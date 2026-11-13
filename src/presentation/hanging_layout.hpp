#pragma once

namespace cliprope::presentation {
// Vertical geometry of one hanging card in DIPs. Values are relative to the
// rope centerline above the hook, except `row`, which is one rope row.
struct Hanging {
    // Leave room above even a narrow rope for its two-line plaque.
    static constexpr float row = 290;
    static constexpr float rope_end = 60, rope_thickness = 9, max_sag = 24;
    static constexpr float hook_top = -9, pivot = -rope_thickness / 2;
    static constexpr float bar_top = 18, bar_end = 25, bar_bottom = 42, bar_overhang = 4;
    static constexpr float paper_top = 42, paper_bottom = 190;
    // The bar's lower edge overlaps the paper so the sheet appears clamped.
    static constexpr float paper_tuck = 4;
    static constexpr float close_size = 26;
    static constexpr float swing_limit = 4.5f; // Degrees in either direction.
};

// A catenary y = end + a(cosh(w / 2a) - cosh((x - w / 2) / a)) anchored at both
// window edges. Its sag grows with width up to a visible but restrained maximum.
class RopeCurve {
  public:
    explicit RopeCurve(float width = 0);
    float y(float x) const;
    float lowest() const { return Hanging::rope_end + sag_; }
    float sag() const { return sag_; }
    float width() const { return width_; }
    // A hook rests on whole DIPs, so card geometry scales exactly with DPI.
    float contact(float hook_x) const;

  private:
    float width_ = 0, sag_ = 0;
    double a_ = 0;
};

struct PointF { float x{}, y{}; };
struct RopePlaque {
    static constexpr float width = 144, height = 48, radius = 5, gap = 10;
    float left, top;
    explicit RopePlaque(const RopeCurve& curve, float row_top = 0)
        : left((curve.width() - width) / 2), top(row_top + curve.lowest() - gap - height) {}
    bool contains(float x, float y) const;
};
// Maps a row-local point into the frame of a card whose hook hangs at
// (hook_x, rope) and is rotated `degrees` clockwise about its pivot.
struct CardFrame {
    float hook_x = 0, rope = 0, degrees = 0;
    void to_card(float x, float y, float &out_x, float &out_y) const;
    PointF from_card(PointF point) const;
};
// Screen-space hook position which pins a grabbed point (in card DIPs) to the cursor.
PointF dragged_hook(PointF cursor_pixels, float pixels_per_dip, PointF grip, float degrees);
} // namespace cliprope::presentation
