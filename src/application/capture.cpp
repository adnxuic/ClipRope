#include "application/capture.hpp"
#include <algorithm>
#include <utility>

namespace cliprope::application {
namespace {
char ascii_lower(char ch) { return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch; }
bool same_app(const std::string& a, const std::string& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(), [](char l, char r) { return ascii_lower(l) == ascii_lower(r); });
}

bool image_snapshot(const domain::Snapshot& snapshot) {
    return std::any_of(snapshot.formats.begin(), snapshot.formats.end(), [](const auto& f) {
        return f.outcome == "saved" && (f.name == "PNG" || f.standard_id == 8 || f.standard_id == 17);
    });
}

// The overlay invocation identifies the capture. A producer can re-encode a
// format on its second publication without creating a second user gesture.
void merge_snip(domain::Snapshot& first, domain::Snapshot& second) {
    std::size_t bytes = 0;
    for (const auto& item : first.formats) bytes += item.bytes.size();
    bool partial = first.capture_status == "partial" || second.capture_status == "partial";
    for (auto& item : second.formats) {
        auto old = first.formats.end();
        for (auto it = first.formats.begin(); it != first.formats.end(); ++it) {
            if (it->standard_id != item.standard_id || it->name != item.name ||
                it->index != item.index || it->aspect != item.aspect) continue;
            old = it;
            if (it->outcome == "saved") break;
        }
        if (old != first.formats.end() && item.outcome != "saved") continue;
        const auto replaced_size = old == first.formats.end() ? 0 : old->bytes.size();
        if (item.bytes.size() > 64u * 1024u * 1024u - (bytes - replaced_size) ||
            (old == first.formats.end() && first.formats.size() == 256)) {
            partial = true;
            continue;
        }
        bytes = bytes - replaced_size + item.bytes.size();
        if (old == first.formats.end()) {
            item.priority = static_cast<int>(first.formats.size());
            first.formats.push_back(std::move(item));
        } else {
            item.priority = old->priority;
            *old = std::move(item);
        }
    }
    if (!second.screenshot_path.empty()) first.screenshot_path = std::move(second.screenshot_path);
    if (first.snip_directory.empty()) first.snip_directory = std::move(second.snip_directory);
    if (!first.snip_begin) first.snip_begin = second.snip_begin;
    first.snip_end = std::max(first.snip_end, second.snip_end);
    first.capture_status = partial ? "partial" : "captured";
}
}

CaptureCoordinator::CaptureCoordinator(History& history)
    : CaptureCoordinator(history, [&history](Admission accepted) { return accepted.commit(history); }) {}

CaptureCoordinator::CaptureCoordinator(History& history, Submit submit)
    : history_(history), submit_(std::move(submit)) {}

std::optional<CaptureTicket> CaptureCoordinator::begin(std::uint32_t version, std::string source, bool self,
                                                      const CapturePolicy& policy, CaptureOutcome& out, std::uint64_t snip_round,
                                                      std::chrono::steady_clock::time_point observed) {
    if (version && last_version_ == version) { out = {"duplicate", {}}; return {}; }
    if (staged_ && staged_->ticket.snip_round != snip_round) ready_ = commit_staged();
    active_.reset();
    last_version_ = version;
    if (!version) out = {"version-unavailable", {}};
    else if (self) out = {"self", {}};
    else if (policy.paused) out = {"paused", {}};
    else if (!policy.excluded_apps.empty() && (source.empty() ||
             std::any_of(policy.excluded_apps.begin(), policy.excluded_apps.end(),
                         [&](const auto& excluded) { return same_app(source, excluded); }))) out = {"excluded-app", {}};
    else if (std::chrono::steady_clock::now() >= observed + std::chrono::seconds(3)) out = {"expired", {}};
    else {
        active_ = CaptureTicket{++serial_, version, history_.active_rope(), std::move(source),
                                observed + std::chrono::seconds(3), snip_round};
        out = {"pending", {}};
    }
    if (!active_ && staged_ && staged_->ticket.snip_round == snip_round &&
        (out.status == "paused" || out.status == "excluded-app")) staged_.reset();
    return active_;
}

CaptureOutcome CaptureCoordinator::finish(const CaptureTicket& ticket, std::uint32_t current_version,
                                         domain::Snapshot snapshot) {
    if (!active_ || ticket.serial != active_->serial) return {"abandoned", {}};
    auto bound = std::move(*active_);
    active_.reset();
    if (bound.version != ticket.version || bound.version != current_version) return {"superseded", {}};
    if (std::chrono::steady_clock::now() >= bound.expires) return {"expired", {}};
    if (staged_ && staged_->ticket.snip_round == bound.snip_round &&
        (snapshot.capture_status == "excluded-marker" || snapshot.capture_status == "privacy-unconfirmed"))
        staged_.reset();
    if (snapshot.capture_status != "partial" && snapshot.capture_status != "captured")
        return {snapshot.capture_status, {}};
    if (std::none_of(snapshot.formats.begin(), snapshot.formats.end(),
                     [](const auto& format) { return format.outcome == "saved" && !format.bytes.empty(); }))
        return {"no-usable-format", {}};
    snapshot.source_hint = bound.source;
    if (bound.snip_round && image_snapshot(snapshot)) {
        if (!staged_) {
            staged_.emplace(Admission{std::move(bound), std::move(snapshot)});
            return {"snip-pending", {}};
        }
        if (staged_->ticket.snip_round == bound.snip_round) {
            merge_snip(staged_->snapshot, snapshot);
            return commit_staged();
        }
    }
    if (staged_) ready_ = commit_staged();
    return submit_({std::move(bound), std::move(snapshot)});
}

CaptureOutcome Admission::commit(History& history) const {
    return commit([&](const auto& rope, const auto& value) { return history.admit(rope, value); });
}
CaptureOutcome Admission::commit(const Write& write) const {
    try { return {snapshot.capture_status, write(ticket.rope, snapshot), {},
                  !snapshot.snip_directory.empty() && snapshot.screenshot_path.empty() && image_snapshot(snapshot)}; }
    catch (const std::exception& error) {
        return {"history-failed", {}, {}, false, 0, history_issue(error)};
    }
}

CaptureOutcome CaptureCoordinator::commit_staged() {
    auto snip = std::move(*staged_);
    staged_.reset();
    return submit_(std::move(snip));
}

std::optional<CaptureOutcome> CaptureCoordinator::settle(bool closing) {
    if (ready_) return std::exchange(ready_, {});
    if (staged_ && (closing || ((!active_ || active_->snip_round != staged_->ticket.snip_round) &&
                                std::chrono::steady_clock::now() >= staged_->ticket.expires)))
        return commit_staged();
    return {};
}

} // namespace cliprope::application
