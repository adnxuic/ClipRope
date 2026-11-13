#pragma once
#include "domain/history.hpp"

namespace cliprope::application {
struct FileGroupMember {
    domain::Id id;
    std::wstring path;
    domain::PathKind kind = domain::PathKind::unknown;
};

// IDs are reserved before creating any provisional Floating windows.
struct ExpandFileGroup {
    domain::Entry source;
    std::string source_hint;
    std::int64_t captured_at{};
    std::vector<FileGroupMember> members;
    bool keep_source = true;
};

struct ExpansionImpact {
    std::size_t required_capacity{}; // Nonzero when the entire batch cannot fit.
    std::vector<domain::Id> removed_tail;
    bool operator==(const ExpansionImpact&) const = default;
};
} // namespace cliprope::application
