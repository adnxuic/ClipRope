#include "presentation/swing.hpp"
#include "presentation/hanging_layout.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cliprope::presentation {
namespace {
std::uint32_t mix(const domain::Id &id) {
    std::uint32_t hash = 2166136261u;
    for (const auto c : id) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 16777619u;
    }
    return hash;
}
double unit(std::uint32_t bits) { return double(bits & 0xff) / 255; }
} // namespace

void Swing::kick(const domain::Id &card, double degrees_per_second) {
    if (card == pinned_ || !std::isfinite(degrees_per_second))
        return;
    auto [it, fresh] = cards_.try_emplace(card);
    auto &m = it->second;
    if (fresh) {
        // Neighbors differ slightly so a scrolled rope does not sway in lockstep.
        const auto h = mix(card);
        m.omega = 7.5 + unit(h);
        m.damping = 0.25 + 0.05 * unit(h >> 8);
        m.gain = 0.85 + 0.3 * unit(h >> 16);
    }
    // An undamped swing started at this speed peaks at the angle limit.
    const double fastest = Hanging::swing_limit * m.omega;
    m.velocity = std::clamp(m.velocity + degrees_per_second * m.gain, -fastest, fastest);
}

void Swing::release(const domain::Id &card, double degrees, double degrees_per_second) {
    if (card == pinned_ || !std::isfinite(degrees) || !std::isfinite(degrees_per_second))
        return;
    cards_.erase(card);
    kick(card, degrees_per_second);
    cards_.at(card).angle = std::clamp(degrees, -double(Hanging::swing_limit), double(Hanging::swing_limit));
}

void Swing::pin(const domain::Id& card) {
    unpin();
    pinned_ = card;
    start_angle_ = angle(card);
    settle_time_ = 0;
    if (start_angle_ == 0) cards_.erase(card);
}

bool Swing::advance(double seconds) {
    if (!std::isfinite(seconds))
        return moving();
    settle_time_ += settling() ? std::max(0., seconds) : 0.;
    seconds = std::clamp(seconds, 0.0, 0.1);
    constexpr double step = 0.004;
    for (auto it = cards_.begin(); it != cards_.end();) {
        auto &m = it->second;
        if (it->first == pinned_) {
            const double t = std::min(1., settle_time_ / .180);
            m.angle = start_angle_ * (1 - t * t * (3 - 2 * t));
            if (t == 1) it = cards_.erase(it);
            else ++it;
            continue;
        }
        for (double left = seconds; left > 1e-9; left -= step) {
            const double dt = std::min(left, step);
            m.velocity -= (m.omega * m.omega * m.angle + 2 * m.damping * m.omega * m.velocity) * dt;
            m.angle += m.velocity * dt;
            if (std::abs(m.angle) > Hanging::swing_limit) {
                m.angle = std::copysign(Hanging::swing_limit, m.angle);
                m.velocity *= -0.3;
            }
        }
        // Below this the paper's lower edge moves less than a quarter DIP.
        if (std::abs(m.angle) < 0.06 && std::abs(m.velocity) < 0.6)
            it = cards_.erase(it);
        else
            ++it;
    }
    return moving();
}

double Swing::angle(const domain::Id &card) const {
    const auto it = cards_.find(card);
    return it == cards_.end() ? 0 : it->second.angle;
}
} // namespace cliprope::presentation
