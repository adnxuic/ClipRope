#pragma once

#include "domain/history.hpp"
#include <unordered_map>

namespace cliprope::presentation {
// Finite pendulum feedback for hanging cards, in degrees with clockwise positive.
// A card is tracked only while it moves, so an idle rope needs no frames.
class Swing {
  public:
    void kick(const domain::Id &card, double degrees_per_second);
    void release(const domain::Id &card, double degrees, double degrees_per_second);
    // Advances every moving card and returns whether any is still moving.
    bool advance(double seconds);
    double angle(const domain::Id &card) const;
    bool moving() const { return !cards_.empty(); }
    bool moving_except(const domain::Id &id) const { return cards_.size() > cards_.count(id); }
    void clear() { cards_.clear(); }
    void pin(const domain::Id& card);
    void unpin() { if (!pinned_.empty()) cards_.erase(pinned_); pinned_.clear(); }
    bool settling() const { return !pinned_.empty() && cards_.contains(pinned_); }

  private:
    struct Motion {
        double angle = 0, velocity = 0, omega = 0, damping = 0, gain = 1;
    };
    std::unordered_map<domain::Id, Motion> cards_;
    domain::Id pinned_;
    double start_angle_{}, settle_time_{};
};
} // namespace cliprope::presentation
