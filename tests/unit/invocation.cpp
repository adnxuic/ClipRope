#include "application/invoke.hpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace cliprope;
using namespace application;
void require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}

struct FakeHistory final : History {
    domain::Entry card{"entry", "snapshot", "A", 0, 1};
    int loads{}, promotions{};
    bool fail{};
    domain::Id active_rope() override { return "B"; }
    domain::Entry entry(std::string_view id) override {
        require(id == card.id, "Unexpected entry lookup");
        return card;
    }
    domain::LeasedSnapshot load_snapshot(std::string_view id) override {
        require(id == card.snapshot_id, "Wrong snapshot loaded");
        ++loads;
        return {};
    }
    void promote(std::string_view id, const domain::Entry* expected) override {
        ++promotions;
        require(expected && expected->id == id && expected->rope_id == card.rope_id, "Promotion lost its bound entry");
        if (fail) throw std::runtime_error("Injected promotion failure");
    }
    domain::LeasedSnapshot load(std::string_view) override { throw std::logic_error("Unexpected load"); }
    domain::Id admit(std::string_view, const domain::Snapshot&) override { throw std::logic_error("Unexpected admission"); }
    void rehang(std::string_view, std::string_view, const domain::Entry*) override { throw std::logic_error("Unexpected return"); }
};

void run() {
    FakeHistory store;
    int published{}, sent{}, collapsed{};
    Publish publish = [&](const auto&) { ++published; return PublicationId{17}; };
    Deliver send = [&](auto receipt) {
        require(receipt == 17 && store.promotions == 1, "Delivery preceded the history attempt");
        ++sent;
        return Delivery::shortcut_sent;
    };
    auto stale = bind_invocation(store, Route::double_click, store.card.id);
    ++store.card.revision;
    auto result = stale.run(store, publish, send);
    require(result.publication == Publication::failed && result.history == HistoryEffect::skipped &&
            result.issue == HistoryIssue::stale_binding && result.delivery == Delivery::skipped &&
            !store.loads && !published && !sent, "Stale gesture caused effects or lost its reason");
    auto rejected = bind_invocation(store, Route::double_click, store.card.id);
    result = rejected.run(store, [&](const auto&) { ++published; return PublicationId{}; }, send);
    require(result.publication == Publication::failed && result.history == HistoryEffect::skipped &&
            published == 1 && !store.promotions && !sent, "Failed publication changed history or delivered input");
    auto failed = bind_invocation(store, Route::double_click, store.card.id);
    store.fail = true;
    result = failed.run(store, publish, send, [&](auto) { ++collapsed; });
    require(result.publication == Publication::published && result.history == HistoryEffect::failed &&
            result.delivery == Delivery::shortcut_sent && !collapsed, "Independent history/delivery receipts lost");
    failed.run(store, publish, send);
    require(published == 2 && store.promotions == 1 && sent == 1, "Spent command repeated an effect");
}
}
int main() {
    try { run(); std::cout << "Pure invocation boundaries and one-shot delivery verified\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
