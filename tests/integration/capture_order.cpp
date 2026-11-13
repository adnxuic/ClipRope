#include "application/capture.hpp"
#include "persistence/history_store.hpp"
#include "history_fixture.hpp"
#include <iostream>

namespace {
using namespace cliprope;
using history_fixture::require;
using persistence::HistoryStore;
using Order = std::vector<domain::Id>;

domain::Snapshot text(char letter) {
    domain::Format format;
    format.standard_id = CF_UNICODETEXT;
    format.bytes = {static_cast<unsigned char>(letter), 0, 0, 0};
    return {{format}};
}

Order order(HistoryStore& store, const domain::Id& rope) {
    Order ids;
    for (const auto& entry : store.hanging(rope)) ids.push_back(entry.id);
    return ids;
}

void capture_after_reorder(const std::filesystem::path& profile, bool reject_admission) {
    domain::Id a, b, survivor, victim, neighbor, capture_id, captured_snapshot;
    domain::Id survivor_snapshot, victim_snapshot, neighbor_snapshot;
    {
        bool reject = false;
        unsigned attempted{}, committed{};
        bool tracing = false;
        HistoryStore store(profile, [&](std::string_view point) {
            if (!tracing) return;
            if (point == "transaction") {
                ++attempted;
                if (reject) throw std::runtime_error("Injected pre-commit failure");
            }
            if (point == "committed") ++committed;
        });
        a = store.active_rope();
        store.set_capacity(a, 2);
        survivor = store.admit(a, text('S'));
        victim = store.admit(a, text('V'));
        b = store.create_rope("Other", 1);
        neighbor = store.admit(b, text('B'));
        survivor_snapshot = store.entry(survivor).snapshot_id;
        victim_snapshot = store.entry(victim).snapshot_id;
        neighbor_snapshot = store.entry(neighbor).snapshot_id;

        application::CaptureCoordinator capture(store);
        application::CaptureOutcome pending;
        const auto ticket = capture.begin(17, "fixture.exe", false, {}, pending);
        require(ticket && ticket->rope == a && pending.status == "pending" && pending.entry.empty(),
                "Notification did not bind the original full rope");
        store.set_active_rope(b);
        reject = tracing = true;
        history_fixture::rejects([&] { store.reorder(victim, 1); }, "Failed reorder committed");
        require(attempted == 1 && committed == 0 && order(store, a) == Order{victim, survivor},
                "Reorder fault changed the eviction target");
        reject = false;
        store.reorder(victim, 1);
        require(order(store, a) == Order{survivor, victim} && committed == 1, "Successful reorder lost the new tail");
        attempted = committed = 0;
        reject = reject_admission;
        const auto result = capture.finish(*ticket, 17, text('X'));
        capture_id = result.entry;
        require(attempted == 1 && committed == (reject ? 0u : 1u), "Unexpected admission transaction count");
        require(capture.finish(*ticket, 17, text('X')).status == "abandoned" && attempted == 1,
                "A terminal ticket ran another admission");
        require(result.status == (reject ? "history-failed" : "captured") && capture_id.empty() == reject,
                "Terminal receipt disagrees with commit");
        if (!reject) captured_snapshot = store.entry(capture_id).snapshot_id;
        require(order(store, a) == (reject ? Order{survivor, victim} : Order{capture_id, survivor}) &&
                order(store, b) == Order{neighbor} && store.active_rope() == b, "Wrong live collector, order or victim");
    }
    HistoryStore reopened(profile);
    require(reopened.active_rope() == b && order(reopened, b) == Order{neighbor} &&
            reopened.entry(neighbor).snapshot_id == neighbor_snapshot &&
            reopened.entry(survivor).snapshot_id == survivor_snapshot, "Unrelated identity changed across restart");
    if (reject_admission) {
        require(order(reopened, a) == Order{survivor, victim} && reopened.entry(victim).snapshot_id == victim_snapshot,
                "Rollback lost the previously committed order or tail snapshot");
    } else {
        require(order(reopened, a) == Order{capture_id, survivor} && reopened.entry(capture_id).snapshot_id == captured_snapshot &&
                reopened.load(capture_id).snapshot.formats.front().bytes == text('X').formats.front().bytes,
                "Restart lost the admitted bytes, identity or order");
        history_fixture::rejects([&] { reopened.entry(victim); }, "Evicted tail still exists");
    }
    std::cout << "reject=" << reject_admission << " bound=" << a << " collector=" << b << " survivor=" << survivor
              << " reordered-tail=" << victim << " other-head=" << neighbor << " admitted=" << capture_id
              << " snapshot=" << captured_snapshot << '\n';
}
}

int main() {
    try {
        history_fixture::Directory dir;
        capture_after_reorder(dir.path / "commit", false);
        capture_after_reorder(dir.path / "rollback", true);
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
