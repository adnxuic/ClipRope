#include "platform_win/search_text.hpp"
#include "history_fixture.hpp"
#include "host/find_worker.hpp"
#include "persistence/history_store.hpp"
#include "persistence/search_fields.hpp"
#include <chrono>
#include <future>
#include <iostream>

namespace {
using namespace cliprope;
using namespace persistence;
using history_fixture::require;
constexpr std::string_view origin = "C:/snips/unique-origin.png";
const application::FindScope scope{"main"};

SearchFields fields(sqlite3* db, const domain::Id& id) {
    Statement rows(db, "SELECT ordinal,kind,search_text FROM search_fields WHERE snapshot_id=? ORDER BY ordinal");
    rows.bind(1, id);
    SearchFields result;
    while (rows.step()) {
        require(rows.integer(0) == static_cast<std::int64_t>(result.size()), "Noncanonical search ordinals");
        result.emplace_back(application::SearchField(rows.integer(1)), rows.wide(2));
    }
    return result;
}

domain::Snapshot pixels() {
    domain::Format png;
    png.name = "PNG";
    png.bytes.assign(16000, 42);
    domain::Snapshot s{{png}};
    s.source_hint = "source-unique-origin.exe";
    return s;
}

void exercise(const std::filesystem::path& root) {
    domain::Id surviving, sibling, snapshot;
    auto canonical = pixels();
    canonical.screenshot_path = origin;
    const auto expected = win::extract_search_fields(canonical);
    {
        HistoryStore store(root);
        auto* db = store.database().handle();
        auto first = store.admit("main", pixels());
        snapshot = store.entry(first).snapshot_id;
        surviving = store.copy(first, "main");
        sibling = store.copy(first, "main");
        store.erase(first);
        const auto revision = store.entry(surviving).revision;
        int changes{};
        store.on_snip_bound = [&] { ++changes; require(fields(db, snapshot) == expected, "Premature notification"); };
        require(store.snip_original(snapshot, origin, {}, false) == origin && store.load(sibling).snapshot.screenshot_path.empty(),
                "Read-only lookup mutated provenance");
        exec(db, "CREATE TEMP TRIGGER reject_origin BEFORE INSERT ON search_fields BEGIN SELECT RAISE(ABORT,'fixture'); END");
        history_fixture::rejects([&] { store.snip_original(snapshot, origin, {}, true); }, "Failed projection did not roll back");
        exec(db, "DROP TRIGGER reject_origin");
        require(!changes && store.load(sibling).snapshot.screenshot_path.empty(), "Failed projection committed provenance");

        require(store.snip_original(snapshot, origin, {}, true) == origin &&
                store.snip_original(snapshot, "different", {}, true) == origin && changes == 1 &&
                store.entry(surviving).revision == revision, "A repeat bind changed identity, revision or notification count");
        CardSearch search(root);
        auto found = search.query(scope, L"unique-origin.png");
        require(found.total == 2 && search.locate(found, 0)->entry == sibling && search.locate(found, 1)->entry == surviving,
                "Live Find missed a shared occurrence or its rope order");

    }
    {
        HistoryStore store(root);
        auto* db = store.database().handle();
        require(fields(db, snapshot) == expected, "Reopen changed the committed index");
        CardSearch search(root);
        require(search.query(scope, L"unique-origin.png").total == 2, "Reopened shared origin missing");
        store.erase(surviving);
        store.erase(sibling);
        require(store.snip_original(snapshot, origin, {}, true).empty() && !search.query(scope, L"unique-origin.png").total,
                "An evicted snapshot was resurrected");

    }
}

void unsupported_worker(const std::filesystem::path& root) {
    {
        Database db(root);
        exec(db.handle(), "PRAGMA user_version=10");
    }
    const auto digest = file_sha256(root / "history.db");
    std::promise<void> ready;
    auto completion = ready.get_future();
    FindWorker worker(root, [&]{ ready.set_value(); });
    worker.submit({scope, L"text", 0, 1});
    require(completion.wait_for(std::chrono::seconds(5)) == std::future_status::ready, "No Find format result");
    auto reply = worker.take();
    require(reply && reply->error == FindWorker::Error::unsupported_format && !reply->hit &&
            digest == file_sha256(root / "history.db"), "Find did not preserve or report an unsupported source");
}

void navigation(const std::filesystem::path& root) {
    HistoryStore store(root);
    const auto id = store.entry(store.admit("main", pixels())).snapshot_id;
    std::mutex mutex;
    std::condition_variable wake;
    bool ready = false;
    FindWorker worker(root, [&] {
        std::lock_guard lock(mutex);
        ready = true;
        wake.notify_one();
    });
    auto request = [&](FindWorker::Request r) {
        worker.submit(std::move(r));
        std::unique_lock lock(mutex);
        require(wake.wait_for(lock, std::chrono::seconds(5), [&] { return ready; }), "Find worker did not complete");
        ready = false;
        lock.unlock();
        auto reply = worker.take();
        require(reply && reply->error == FindWorker::Error::none && reply->hit, "Find returned no active hit");
        return *reply;
    };
    auto before = request({scope, L"unique-origin", 0, 1});
    require(before.total == 1 && before.hit->field == application::SearchField::metadata, "Incorrect initial source hit");
    store.snip_original(id, origin, {}, true);
    auto after = request({scope, L"unique-origin", 0, 2, before.hit});
    require(after.total == 2 && after.index == 1 && after.hit->field == application::SearchField::metadata &&
            after.hit->ordinal == before.hit->ordinal + 1, "Refreshing navigated away from the active source");
    auto path = request({scope, L"unique-origin", 0, 2});
    require(path.hit->field == application::SearchField::path && path.hit->context.find(L".png") != std::wstring::npos,
            "Find reused a cached source field for the new path");
}

}
int main() {
    try {
        history_fixture::Directory dir;
        exercise(dir.path / "fields");
        unsupported_worker(dir.path / "unsupported");
        navigation(dir.path / "find");
        std::cout << "Origin atomicity, live navigation and shared restart verified\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
