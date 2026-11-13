#pragma once
#include "application/history_failure.hpp"
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace cliprope::application {
// Only aggregate categories cross the diagnostic boundary. Source names,
// format names, entry IDs, paths, clipboard bodies and exception text stay out.
struct Diagnostics {
    std::uint64_t captured{}, skipped{}, partial{}, failures{}, cleanup_deferred{};
    std::uint64_t target_timeouts{};
    enum class Stage { capture, command };
    struct Issues { std::uint64_t stale{}, busy{}, storage{}; };
    std::array<Issues, 2> issues{};
    void history(Stage stage, HistoryIssue issue) {
        auto& counts = issues[static_cast<std::size_t>(stage)];
        switch (issue) {
        case HistoryIssue::none: break;
        case HistoryIssue::stale_binding: ++counts.stale; break;
        case HistoryIssue::storage_busy: ++counts.busy; break;
        case HistoryIssue::storage_failed: ++counts.storage; break;
        }
    }
    void capture(std::string_view result, HistoryIssue issue = {}) {
        if (result == "pending" || result == "snip-pending" || result == "admission-pending")
            return;
        if (result == "captured")
            ++captured;
        else if (result == "partial")
            ++partial;
        else if (result == "self" || result == "duplicate" || result == "paused" || result == "excluded-app" ||
                 result == "excluded-marker" || result == "abandoned" || result == "superseded")
            ++skipped;
        else {
            ++failures;
            history(Stage::capture, issue);
        }
    }
    std::string report(std::uint64_t observer_failures = 0) const {
        auto counts = [](const Issues& values) {
            return "stale=" + std::to_string(values.stale) + " busy=" + std::to_string(values.busy) +
                   " storage=" + std::to_string(values.storage);
        };
        return "ClipRope diagnostics v2\r\nScope: current session\r\n"
               "Captured: " +
               std::to_string(captured) + "\r\nPartial: " + std::to_string(partial) +
               "\r\nSkipped: " + std::to_string(skipped) + "\r\nFailures: " + std::to_string(failures) +
               "\r\nCapture history: " + counts(issues[0]) + "\r\nCommand history: " + counts(issues[1]) +
               "\r\nPost-commit notification failures: " + std::to_string(observer_failures) +
               "\r\nTarget verification timeouts: " + std::to_string(target_timeouts) +
               "\r\nDeferred cleanup files (last sweep): " + std::to_string(cleanup_deferred) +
               "\r\nFor missing content, keep the profile and restore a verified backup to an empty "
               "folder.\r\n";
    }
};
} // namespace cliprope::application
