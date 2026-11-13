#include "presentation/hanging_layout.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace cliprope::presentation {
namespace {
// The sag reaches its 24-DIP cap across a 1920-pixel display at 125%.
constexpr float sag_per_width = 1.f / 64;
} // namespace

RopeCurve::RopeCurve(float width) : width_(std::isfinite(width) ? std::max(0.f, width) : 0.f) {
    sag_ = std::min(Hanging::max_sag, width_ * sag_per_width);
    if (sag_ < 0.01f)
        return;
    const double half = width_ / 2.0, sag = sag_;
    // Newton's method from the parabolic estimate converges in a few steps
    // because the sag is a small fraction of the span.
    double a = half * half / (2 * sag);
    for (int i = 0; i < 32; ++i) {
        const double u = half / a;
        const double f = a * (std::cosh(u) - 1) - sag;
        const double slope = std::cosh(u) - 1 - u * std::sinh(u);
        if (slope == 0)
            break;
        const double next = a - f / slope;
        if (!(next > 0) || !std::isfinite(next))
            break;
        const bool done = std::abs(next - a) < 1e-9 * a;
        a = next;
        if (done)
            break;
    }
    a_ = a;
}

float RopeCurve::y(float x) const {
    if (!a_)
        return Hanging::rope_end + sag_;
    const double half = width_ / 2.0;
    const double t = std::clamp(double(x), 0.0, double(width_));
    return static_cast<float>(Hanging::rope_end + a_ * (std::cosh(half / a_) - std::cosh((t - half) / a_)));
}

float RopeCurve::contact(float hook_x) const { return std::round(y(hook_x)); }

bool RopePlaque::contains(float x, float y) const {
    x -= left;
    y -= top;
    if (x < 0 || x >= width || y < 0 || y >= height) return false;
    const float dx = x - std::clamp(x, radius, width - radius);
    const float dy = y - std::clamp(y, radius, height - radius);
    return dx * dx + dy * dy <= radius * radius;
}

void CardFrame::to_card(float x, float y, float &out_x, float &out_y) const {
    const double radians = -double(degrees) * std::numbers::pi / 180;
    const double dx = x - hook_x, dy = y - rope - Hanging::pivot;
    out_x = static_cast<float>(dx * std::cos(radians) - dy * std::sin(radians));
    out_y = static_cast<float>(dx * std::sin(radians) + dy * std::cos(radians) + Hanging::pivot);
}
PointF CardFrame::from_card(PointF point) const {
    const double radians = double(degrees) * std::numbers::pi / 180;
    const double dy = point.y - Hanging::pivot;
    return {float(hook_x + point.x * std::cos(radians) - dy * std::sin(radians)),
            float(rope + Hanging::pivot + point.x * std::sin(radians) + dy * std::cos(radians))};
}
PointF dragged_hook(PointF cursor_pixels, float pixels_per_dip, PointF grip, float degrees) {
    const auto offset = CardFrame{0, 0, degrees}.from_card(grip);
    return {cursor_pixels.x - offset.x * pixels_per_dip, cursor_pixels.y - offset.y * pixels_per_dip};
}
} // namespace cliprope::presentation
