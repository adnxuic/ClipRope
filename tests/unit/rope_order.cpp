#include "domain/history.hpp"
#include <iostream>
#include <stdexcept>

using namespace cliprope::domain;
namespace {
void require(bool passed) {
    if (!passed) throw std::runtime_error("Rope ordering contract failed");
}
}

int main() {
    try {
        RopeOrder promoted({"A", "B", "C"});
        promoted.reorder("C", 0);
        promoted.insert_head("D");
        require(promoted.trim(3) == std::vector<Id>{"B"});
        require(promoted.ids() == std::vector<Id>{"D", "C", "A"});

        RopeOrder reordered({"A", "B", "C"});
        reordered.reorder("A", 2);
        reordered.insert_head("D");
        require(reordered.trim(3) == std::vector<Id>{"A"});
        require(reordered.ids() == std::vector<Id>{"D", "B", "C"});

        Entry card{"C", "immutable", "rope", 2, 1};
        reordered.detach(card.id);
        require(card.detach() == "rope");
        reordered.insert_head("E");
        require(reordered.trim(3).empty());
        require(reordered.trim(1) == std::vector<Id>{"D", "B"});
        require(!card.rope_id && !card.sort_key && card.id == "C" && card.snapshot_id == "immutable");
        try {
            reordered.trim(0);
            return 1;
        } catch (const std::invalid_argument&) {}
        require(reordered.ids() == std::vector<Id>{"E"});
        std::cout << "Promotion, reorder, tail eviction, Floating exemption and capacity checks passed\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
