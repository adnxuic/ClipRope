#pragma once

#include "application/history.hpp"
#include "application/history_failure.hpp"
#include "application/collector_head.hpp"
#include <functional>
#include <utility>

namespace cliprope::application {

enum class Route { double_click, tray, rope_head, return_button };
enum class Publication { skipped, failed, published };
enum class Delivery { skipped, no_field, shortcut_sent, lost_target, denied, replaced, unconfirmed, send_failed };
enum class HistoryEffect { skipped, failed, committed };
struct InvocationResult {
    Publication publication = Publication::skipped;
    Delivery delivery = Delivery::skipped;
    HistoryEffect history = HistoryEffect::skipped;
    HistoryIssue issue{};
};

// Zero denotes a failed publication. The publisher authenticates every other receipt.
using PublicationId = std::uint64_t;
using Publish = std::function<PublicationId(const domain::LeasedSnapshot&)>;
Publication publish_head_change(const HeadChange& change, const CardPreferences& preferences,
                                History& store, const Publish& publish);
using Deliver = std::function<Delivery(PublicationId)>;
const char* delivery_name(Delivery value);

class Invocation {
public:
    Invocation(const Invocation&) = delete;
    Invocation& operator=(const Invocation&) = delete;
    Invocation(Invocation&& other) noexcept : route_(other.route_), entry_(std::move(other.entry_)),
        target_(std::move(other.target_)), paste_(other.paste_), spent_(std::exchange(other.spent_, true)) {}
    static Invocation hanging_double_click(domain::Entry pressed, bool paste = true);
    const domain::Entry& bound_entry() const { return entry_; }
    InvocationResult run(History& store, const Publish& publish, const Deliver& deliver = {},
                         const std::function<void(PublicationId)>& on_commit = {},
                         PreparedSnapshot* prepared = nullptr);
private:
    friend Invocation bind_invocation(History&, Route, std::string_view, std::optional<domain::Id>);
    Invocation(Route route, domain::Entry entry, domain::Id target, bool paste = false)
        : route_(route), entry_(std::move(entry)), target_(std::move(target)), paste_(paste) {}
    Route route_;
    domain::Entry entry_;
    domain::Id target_;
    bool paste_{};
    bool spent_{};
};

Invocation bind_invocation(History& store, Route route, std::string_view id,
                           std::optional<domain::Id> frozen_return_rope = {});

} // namespace cliprope::application
