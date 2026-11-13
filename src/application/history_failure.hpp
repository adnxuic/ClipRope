#pragma once

#include <stdexcept>

namespace cliprope::application {
enum class HistoryIssue { none, stale_binding, storage_busy, storage_failed };

class HistoryFailure : public std::runtime_error {
public:
    HistoryFailure(HistoryIssue issue, const char* message) : std::runtime_error(message), issue(issue) {}
    HistoryIssue issue;
};

// Missing bound IDs keep the existing out_of_range contract of the store.
class MissingHistoryTarget final : public std::out_of_range {
public:
    using std::out_of_range::out_of_range;
};

inline HistoryIssue history_issue(const std::exception& error) noexcept {
    if (const auto* failure = dynamic_cast<const HistoryFailure*>(&error)) return failure->issue;
    if (dynamic_cast<const MissingHistoryTarget*>(&error)) return HistoryIssue::stale_binding;
    return HistoryIssue::none;
}
}
