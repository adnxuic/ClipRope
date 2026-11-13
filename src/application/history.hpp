#pragma once

#include "domain/history.hpp"
#include <string_view>

namespace cliprope::application {

struct PreparedSnapshot {
    domain::Id snapshot_id;
    domain::LeasedSnapshot content;
};

// Commands use IDs bound at action time. Invalid or stale targets throw.
// A successful mutation has committed before its caller updates a view.
class History {
public:
    virtual ~History() = default;
    virtual domain::Id active_rope() = 0;
    virtual domain::Entry entry(std::string_view id) = 0;
    virtual domain::LeasedSnapshot load(std::string_view entry_id) = 0;
    virtual domain::LeasedSnapshot load_snapshot(std::string_view snapshot_id) = 0;
    virtual domain::Id admit(std::string_view rope_id, const domain::Snapshot& snapshot) = 0;
    virtual void promote(std::string_view entry_id, const domain::Entry* expected = nullptr) = 0;
    virtual void rehang(std::string_view entry_id, std::string_view bound_rope_id,
                        const domain::Entry* expected = nullptr) = 0;
};

} // namespace cliprope::application
