#include "application/invoke.hpp"
#include <stdexcept>

namespace cliprope::application {
Publication publish_head_change(const HeadChange& change, const CardPreferences& preferences,
                                History& store, const Publish& publish) {
    if (!change.follows(preferences)) return Publication::skipped;
    try {
        if (publish(store.load_snapshot(change.after.snapshot))) return Publication::published;
    } catch (const std::exception&) {}
    return Publication::failed;
}

Invocation Invocation::hanging_double_click(domain::Entry pressed, bool paste) {
    if (!pressed.rope_id) throw std::invalid_argument("Clipboard recall requires a Hanging card");
    return Invocation(Route::double_click, std::move(pressed), {}, paste);
}

Invocation bind_invocation(History& store, Route route, std::string_view id,
                           std::optional<domain::Id> frozen_return_rope) {
    auto entry = store.entry(id);
    if (route == Route::double_click) return Invocation::hanging_double_click(std::move(entry));
    if ((route == Route::tray || route == Route::rope_head) && !entry.rope_id)
        throw std::invalid_argument("The selected entry is not Hanging");
    if (route == Route::return_button && entry.rope_id) throw std::invalid_argument("The selected entry is not Floating");
    domain::Id target;
    if (!entry.rope_id) target = frozen_return_rope ? std::move(*frozen_return_rope) : store.active_rope();
    return Invocation(route, std::move(entry), std::move(target));
}

const char* delivery_name(Delivery value) {
    static constexpr const char* names[] = {"skipped", "no-field", "shortcut_sent", "lost-target", "denied",
                                            "replaced", "unconfirmed", "send-failed"};
    return names[static_cast<unsigned>(value)];
}

InvocationResult Invocation::run(History& store, const Publish& publish, const Deliver& deliver,
                                 const std::function<void(PublicationId)>& on_commit,
                                 PreparedSnapshot* prepared) {
    InvocationResult result;
    if (spent_) return result;
    spent_ = true;
    PublicationId receipt{};
    if (route_ != Route::return_button) {
        domain::LeasedSnapshot content;
        result.publication = Publication::failed;
        try {
            const auto current = store.entry(entry_.id);
            if (current.snapshot_id != entry_.snapshot_id || current.rope_id != entry_.rope_id ||
                current.revision != entry_.revision || (prepared && prepared->snapshot_id != entry_.snapshot_id)) {
                result.issue = HistoryIssue::stale_binding;
                return result;
            }
            content = prepared ? std::move(prepared->content) : store.load_snapshot(entry_.snapshot_id);
            if (publish) receipt = publish(content);
            if (!receipt) return result;
        } catch (const std::exception& error) { result.issue = history_issue(error); return result; }
        result.publication = Publication::published;
    }
    try {
        if (entry_.rope_id) store.promote(entry_.id, &entry_);
        else store.rehang(entry_.id, target_, &entry_);
        result.history = HistoryEffect::committed;
    } catch (const std::exception& error) {
        result.history = HistoryEffect::failed;
        result.issue = history_issue(error);
    }
    if (result.history == HistoryEffect::committed && on_commit) on_commit(receipt);
    if (paste_) {
        try { result.delivery = deliver ? deliver(receipt) : Delivery::no_field; }
        catch (const std::exception&) { result.delivery = Delivery::unconfirmed; }
    }
    return result;
}
} // namespace cliprope::application
