#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cliprope::application {
struct PreviewChanges {
    std::vector<std::string> blobs;
    std::uint64_t generation{};
    bool all{};
    void add(std::string_view id, std::uint64_t current) {
        if (all) return;
        if ((generation && generation != current) || id.empty()) { all = true; blobs.clear(); return; }
        generation = current;
        if (contains(id)) return;
        if (blobs.size() == 64) { all = true; blobs.clear(); return; }
        blobs.emplace_back(id);
    }
    bool contains(std::string_view id) const {
        return all || (!id.empty() && std::find(blobs.begin(), blobs.end(), id) != blobs.end());
    }
};
}
