#pragma once

#include "application/history.hpp"
#include "application/history_failure.hpp"
#include <chrono>
#include <functional>

namespace cliprope::application {

struct CapturePolicy {
    bool paused{};
    std::vector<std::string> excluded_apps; // Executable basenames.
};

struct CaptureTicket {
    std::uint64_t serial;
    std::uint32_t version;
    domain::Id rope;
    std::string source;
    std::chrono::steady_clock::time_point expires;
    std::uint64_t snip_round{};
};

struct CaptureOutcome {
    std::string status;
    domain::Id entry;
    std::string detail; // Content-free adapter summary.
    bool locate_snip{};
    std::uint64_t admission_serial{};
    HistoryIssue issue{};
};

struct Admission {
    using Write = std::function<domain::Id(const domain::Id&, const domain::Snapshot&)>;
    CaptureTicket ticket;
    domain::Snapshot snapshot;
    CaptureOutcome commit(History& history) const;
    CaptureOutcome commit(const Write& write) const;
};

// The listener and completion dispatcher call this on one command executor.
// Each new clipboard version supersedes its predecessor, including excluded events.
class CaptureCoordinator {
public:
    using Submit = std::function<CaptureOutcome(Admission)>;
    explicit CaptureCoordinator(History& history);
    CaptureCoordinator(History& history, Submit submit);
    std::optional<CaptureTicket> begin(std::uint32_t version, std::string source, bool self,
                                       const CapturePolicy& policy, CaptureOutcome& outcome, std::uint64_t snip_round = 0,
                                       std::chrono::steady_clock::time_point observed = std::chrono::steady_clock::now());
    CaptureOutcome finish(const CaptureTicket& ticket, std::uint32_t current_version, domain::Snapshot snapshot);
    bool current(const CaptureTicket& ticket, std::uint32_t version) const {
        return active_ && ticket.serial == active_->serial && ticket.version == version;
    }
    std::optional<CaptureOutcome> settle(bool closing = false);
    bool settling() const { return staged_.has_value() || ready_.has_value(); }
    void abandon() { active_.reset(); staged_.reset(); ready_.reset(); }
private:
    CaptureOutcome commit_staged();
    std::optional<Admission> staged_;
    std::optional<CaptureOutcome> ready_;
    History& history_;
    Submit submit_;
    std::uint32_t last_version_{};
    std::uint64_t serial_{};
    std::optional<CaptureTicket> active_;
};

} // namespace cliprope::application
