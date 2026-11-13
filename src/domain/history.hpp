#pragma once
#include "domain/color.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cliprope::domain {

using Id = std::string;
inline constexpr char rebin_id[] = "system:rebin";

struct Format {
    std::uint32_t standard_id = 0; // Zero selects the registered name.
    std::string name;
    std::vector<unsigned char> bytes;
    std::string outcome = "saved";
    std::uint32_t tymed = 1, aspect = 1;
    std::int32_t index = -1, codec_version = 1, priority = 0;
};

enum class PathKind { unknown, file, directory };
struct Snapshot {
    std::vector<Format> formats;
    std::string source_hint;
    std::string capture_status = "captured";
    std::int64_t captured_at = 0;
    std::string screenshot_path; // UTF-8, bound to this capture only.
    PathKind path_kind = PathKind::unknown;
    std::string snip_directory;
    std::int64_t snip_begin{}, snip_end{}; // Windows adapter's capture interval, in FILETIME ticks.
};

struct LeasedSnapshot {
    Snapshot snapshot;
    std::shared_ptr<void> lease;
};

struct Rope {
    Id id;
    std::string name;
    std::optional<int> capacity = 100;
    bool is_main = false;
    ColorSelection color;
    RopeMaterial material;
    bool protected_rope() const { return is_main || id == rebin_id; }
    std::size_t slot_limit() const {
        return capacity ? static_cast<std::size_t>(*capacity) : std::numeric_limits<std::size_t>::max();
    }
};

struct CardScale {
    static constexpr float minimum = 0.75f, maximum = 2.f;
    float width = 1, height = 1;
    bool operator==(const CardScale&) const = default;
};

struct Layout {
    std::string monitor;
    double x = 0, y = 0, width = 240, height = 160;
    bool visible = true;
    bool operator==(const Layout&) const = default;
};

struct Entry {
    Id id, snapshot_id;
    std::optional<Id> rope_id; // Empty for Floating.
    std::optional<std::int64_t> sort_key;
    std::int64_t revision = 1;

    // Release both membership fields together and return the affected rope.
    Id detach();
};

// An authoritative, displayed order for one rope. Floating entries never
// enter its constructor. Mutations return identities for the writer to persist.
class RopeOrder {
public:
    explicit RopeOrder(std::vector<Id> hanging = {});
    const std::vector<Id>& ids() const { return ids_; }
    void insert_head(const Id& id);
    void reorder(const Id& id, std::size_t zero_based_position);
    void detach(const Id& id);
    std::vector<Id> trim(int capacity);
private:
    std::vector<Id> ids_;
};

} // namespace cliprope::domain
