#include "domain/history.hpp"
#include <algorithm>
#include <stdexcept>
#include <utility>

namespace cliprope::domain {

Id Entry::detach() {
    if (!rope_id || !sort_key) throw std::invalid_argument("Detach requires a Hanging card");
    Id previous = std::move(*rope_id);
    rope_id.reset();
    sort_key.reset();
    return previous;
}

RopeOrder::RopeOrder(std::vector<Id> hanging) : ids_(std::move(hanging)) {}

void RopeOrder::insert_head(const Id& id) {
    if (id.empty() || std::find(ids_.begin(), ids_.end(), id) != ids_.end())
        throw std::invalid_argument("Head admission requires a new membership");
    ids_.insert(ids_.begin(), id);
}

void RopeOrder::reorder(const Id& id, std::size_t position) {
    if (position >= ids_.size()) throw std::out_of_range("Rope position is out of range");
    const auto current = std::find(ids_.begin(), ids_.end(), id);
    if (current == ids_.end()) throw std::invalid_argument("Entry is not Hanging on this rope");
    const auto target = ids_.begin() + static_cast<std::ptrdiff_t>(position);
    if (current < target) std::rotate(current, current + 1, target + 1);
    if (current > target) std::rotate(target, current, current + 1);
}

void RopeOrder::detach(const Id& id) {
    const auto it = std::find(ids_.begin(), ids_.end(), id);
    if (it == ids_.end()) throw std::invalid_argument("Entry is not Hanging on this rope");
    ids_.erase(it);
}

std::vector<Id> RopeOrder::trim(int capacity) {
    if (capacity < 1) throw std::invalid_argument("Rope capacity must be positive");
    if (ids_.size() <= static_cast<std::size_t>(capacity)) return {};
    const auto first = ids_.begin() + capacity;
    std::vector<Id> evicted(first, ids_.end());
    ids_.erase(first, ids_.end());
    return evicted;
}

} // namespace cliprope::domain
