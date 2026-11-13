#include "history_fixture.hpp"
#include "persistence/history_store.hpp"
#include <atomic>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>

namespace {
using namespace cliprope;
using namespace persistence;
using namespace std::chrono_literals;
using history_fixture::require;
namespace fs = std::filesystem;

domain::Snapshot sample(unsigned char value, size_t bytes = 20000) {
    domain::Format f;
    f.name = "Backup fixture";
    f.bytes.assign(bytes, value);
    return {{f}};
}
std::string digest(HistoryStore &store, const domain::Id &id) {
    return sha256(store.load(id).snapshot.formats.at(0).bytes);
}
std::vector<domain::Id> order(HistoryStore &store, const domain::Id &rope) {
    std::vector<domain::Id> result;
    for (const auto &e : store.hanging(rope))
        result.push_back(e.id);
    return result;
}

void deleted_content(const fs::path &dir) {
    const auto profile = dir / "live", backup = dir / "backup", restored = dir / "restored";
    // Each distinct value is synthetic. The inline sentinel is also searched
    // for in the raw exported files before opening the backup as a store.
    const std::string marker = "CLIPROPE-SYNTHETIC-DELETED-INLINE-9d1290458c40";
    auto discarded = sample(133);
    discarded.formats[0].name = "Missing obsolete external";
    auto remaining = sample(134).formats[0];
    remaining.name = "Present obsolete external";
    domain::Format secret;
    secret.name = "Discarded inline";
    secret.bytes.assign(marker.begin(), marker.end());
    auto live_format = sample(99).formats[0];
    discarded.formats.insert(discarded.formats.end(), {remaining, secret, live_format});
    const auto missing_hash = sha256(discarded.formats.front().bytes);
    const auto present_hash = sha256(remaining.bytes), live_hash = sha256(live_format.bytes);
    std::string id;
    {
        HistoryStore store(profile);
        id = store.admit("main", {{live_format}});
        const auto removed = store.admit("main", discarded);
        store.erase(removed);
        Statement stale(store.database().handle(),
                        "SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM representations),"
                        "(SELECT count(*) FROM blobs),(SELECT count(*) FROM entries)");
        require(stale.step() && stale.integer(0) == 2 && stale.integer(1) == 5 && stale.integer(2) == 4 &&
                    stale.integer(3) == 1,
                "Deleted-content fixture was collected before export");
        require(fs::remove(safe_blob_path(profile, missing_hash)) &&
                    fs::exists(safe_blob_path(profile, present_hash)),
                "Could not set up missing and intact unreachable external files");
        store.database().backup_to(backup);
    }
    require(fs::exists(safe_blob_path(profile, present_hash)), "Backup collected the source");
    Database::verify_store(profile);
    require(!fs::exists(safe_blob_path(backup, missing_hash)) &&
                !fs::exists(safe_blob_path(backup, present_hash)) &&
                fs::exists(safe_blob_path(backup, live_hash)),
            "Export copied an unreachable external or lost shared live content");
    for (const auto &item : fs::recursive_directory_iterator(backup)) {
        if (!item.is_regular_file())
            continue;
        std::ifstream input(item.path(), std::ios::binary);
        require(input.good(), "Cannot scan exported test fixture");
        const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
        require(bytes.find(marker) == std::string::npos,
                "Deleted inline sentinel survived in an exported file");
    }
    {
        const auto db_file = (backup / "history.db").u8string();
        sqlite3 *raw{};
        require(sqlite3_open_v2(reinterpret_cast<const char *>(db_file.c_str()), &raw, SQLITE_OPEN_READONLY,
                                nullptr) == SQLITE_OK,
                "Could not inspect unswept backup");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        Statement rows(raw, "SELECT (SELECT count(*) FROM snapshots),(SELECT count(*) FROM representations),"
                            "(SELECT count(*) FROM blobs),(SELECT count(*) FROM entries)");
        require(rows.step() && rows.integer(0) == 1 && rows.integer(1) == 1 && rows.integer(2) == 1 &&
                    rows.integer(3) == 1,
                "Fresh export still contains unreferenced content rows");
    }
    Database::restore_backup(backup, restored);
    HistoryStore recovered(restored);
    require(order(recovered, "main") == std::vector<domain::Id>{id} && digest(recovered, id) == live_hash,
            "Restore could not recover the surviving entry and shared bytes");
    std::cout << "source_gc=deferred missing_and_present_orphan_files_excluded=2 "
                 "deleted_inline_marker_absent_from_export=1 snapshot_and_representation_orphans=0 "
                 "live_shared_hash_restored=1\n";
}

void run(const fs::path &dir) {
    std::promise<void> pinned, release;
    auto signal = pinned.get_future();
    auto resume = release.get_future();
    const auto live = dir / "live", backup = dir / "snapshot", restored_path = dir / "restored";
    HistoryStore store(live, [&](std::string_view boundary) {
        if (boundary == "backup-pinned") {
            pinned.set_value();
            if (resume.wait_for(6s) != std::future_status::ready)
                throw std::runtime_error("Writer did not make progress during export");
        }
    });
    const auto second = store.create_rope("Imported", 3);
    store.set_active_rope(second);
    const auto shared = store.admit("main", sample(1)), inline_id = store.admit("main", sample(2, 16));
    const auto duplicate = store.copy(shared, second), floating = store.admit(second, sample(3));
    store.detach(floating, {"Synthetic negative monitor", -92, 41, 312, 198});
    const auto shared_snapshot = store.entry(shared).snapshot_id,
               floating_snapshot = store.entry(floating).snapshot_id;
    const auto shared_hash = digest(store, shared), floating_hash = digest(store, floating),
               inline_hash = digest(store, inline_id);
    auto export_job = std::async(std::launch::async, [&] { store.database().backup_to(backup); });
    require(signal.wait_for(6s) == std::future_status::ready, "Export did not freeze a view");
    store.erase(shared);
    store.erase(duplicate);
    store.erase(floating);
    store.erase(inline_id);
    const auto later = store.admit("main", sample(4));
    store.set_active_rope("main");
    store.gc();
    require(store.hanging("main").size() == 1 && store.hanging("main")[0].id == later &&
                store.floating().empty(),
            "Writer did not commit independent history while export was paused");
    {
        Statement removed(store.database().handle(), "SELECT count(*) FROM blobs WHERE id IN (?,?)");
        removed.bind(1, shared_hash);
        removed.bind(2, floating_hash);
        require(removed.step() && removed.integer(0) == 0, "Live GC did not reclaim the two orphan SQL rows");
    }
    require(fs::exists(safe_blob_path(live, shared_hash)) && fs::exists(safe_blob_path(live, floating_hash)),
            "Live GC removed files leased by the frozen manifest");
    release.set_value();
    export_job.get();
    store.gc();
    require(!fs::exists(safe_blob_path(live, shared_hash)) && !fs::exists(safe_blob_path(live, floating_hash)),
            "Released backup leases did not become reclaimable");
    require(fs::create_directory(restored_path), "Could not create empty restore destination");
    Database::restore_backup(backup, restored_path);
    {
        HistoryStore restored(restored_path);
        require(restored.active_rope() == second &&
                    order(restored, "main") == std::vector<domain::Id>{inline_id, shared} &&
                    order(restored, second) == std::vector<domain::Id>{duplicate},
                "Restore order or collector differs from the snapshot");
        const auto f = restored.floating();
        const auto layout = restored.layout(floating);
        require(f.size() == 1 && f[0].id == floating && f[0].snapshot_id == floating_snapshot &&
                    !f[0].rope_id && !f[0].sort_key && layout.monitor == "Synthetic negative monitor" &&
                    layout.x == -92 && layout.y == 41 && layout.width == 312 && layout.height == 198 &&
                    layout.visible,
                "Independent Floating geometry or membership changed");
        require(restored.entry(shared).snapshot_id == shared_snapshot &&
                    restored.entry(duplicate).snapshot_id == shared_snapshot &&
                    digest(restored, shared) == shared_hash && digest(restored, duplicate) == shared_hash &&
                    digest(restored, inline_id) == inline_hash && digest(restored, floating) == floating_hash,
                "Restored bytes or snapshot sharing changed");
    }
    history_fixture::rejects([&] { Database::restore_backup(backup, restored_path); },
                             "Restore replaced a nonempty profile");
    const auto broken = dir / "broken-backup", uninstalled = dir / "empty";
    fs::copy(backup, broken, fs::copy_options::recursive);
    fs::create_directory(uninstalled);
    std::ofstream(safe_blob_path(broken, floating_hash), std::ios::binary | std::ios::trunc).put('!');
    bool refused = false;
    try {
        Database::restore_backup(broken, uninstalled);
    } catch (const StorageFailure &e) {
        refused = e.reason() == StorageProblem::damaged_blob;
    }
    require(refused && fs::is_empty(uninstalled), "Damaged export was installed");
    Database::verify_store(backup);
    std::cout << "wal_read_snapshot=1 writer_and_gc_completed=1 protected_external_files=2 "
                 "distinct_shared_entries=2 floating_layout=preserved damaged_restore=refused\n";
}
void cli(const fs::path &exe, const fs::path &source, std::wstring_view op, const fs::path &dest = {}, DWORD expected = 0) {
    std::wstring args = L"\"" + exe.wstring() + L"\" \"" + source.wstring() + L"\" " + std::wstring(op);
    if (!dest.empty())
        args += L" \"" + dest.wstring() + L"\"";
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION child{};
    require(CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &startup, &child),
            "Recovery command could not start");
    CloseHandle(child.hThread);
    const auto waited = WaitForSingleObject(child.hProcess, 15000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(child.hProcess, 2);
        WaitForSingleObject(child.hProcess, 2000);
    }
    DWORD result = 1;
    GetExitCodeProcess(child.hProcess, &result);
    CloseHandle(child.hProcess);
    require(waited == WAIT_OBJECT_0 && result == expected, "Recovery command returned the wrong result");
}
template<class F> void manifest_rejected(F action) {
    try { action(); }
    catch (const StorageFailure& error) {
        require(error.reason() == StorageProblem::damaged_manifest, "Incorrect sealed-manifest failure class");
        return;
    }
    throw std::runtime_error("Corrupt sealed snapshot accepted");
}
void manifest_case(const fs::path& dir, const fs::path& exe) {
    const auto source = dir / "source", healthy = dir / "healthy", empty = dir / "empty", refused = dir / "refused";
    fs::create_directory(empty);
    domain::Id id, floating;
    const auto bytes = sample(19);
    {
        HistoryStore store(source);
        id = store.admit("main", bytes);
        floating = store.copy(id, "main");
        store.detach(floating);
        auto* db = store.database().handle();
        store.database().backup_to(healthy);
        // Only a legal INTEGER changes. Content length, blob hash and references still agree.
        exec(db, "UPDATE representations SET priority=priority+1");
        manifest_rejected([&] { store.load(floating); });
        manifest_rejected([&] { store.database().backup_to(refused); });
        require(!fs::exists(refused), "Failed live backup installed a destination");
    }
    const auto source_hash = file_sha256(source / "history.db");
    manifest_rejected([&] { Database::verify_store(source); });
    manifest_rejected([&] { Database::export_profile(source, refused); });
    manifest_rejected([&] { Database::restore_backup(source, empty); });
    cli(exe, source, L"verify", {}, 1);
    cli(exe, source, L"backup", refused, 1);
    cli(exe, source, L"restore", empty, 1);
    require(file_sha256(source / "history.db") == source_hash && fs::is_empty(empty) && !fs::exists(refused),
            "Failed verification changed the source or installed unverified data");
    Database::verify_store(healthy);
    Database::restore_backup(healthy, empty);
    HistoryStore restored(empty);
    require(restored.entry(id).snapshot_id == restored.entry(floating).snapshot_id &&
            !restored.entry(floating).rope_id && restored.load(id).snapshot.formats[0].bytes == bytes.formats[0].bytes,
            "Shared or Floating recovery lost content");
    {
        HistoryStore store(source);
        exec(store.database().handle(), "UPDATE snapshots SET manifest_hash=''");
        manifest_rejected([&] { store.load(id); });
        manifest_rejected([&] { store.database().backup_to(refused); });
    }
    const auto unsealed_hash = file_sha256(source / "history.db");
    manifest_rejected([&] { Database::verify_store(source); });
    manifest_rejected([&] { Database::restore_backup(source, refused); });
    require(!fs::exists(refused) && file_sha256(source / "history.db") == unsealed_hash,
            "Rejected unsealed content changed the source or destination");
    std::cout << "Missing seals and priority-only tampering rejected; healthy backup restored\n";
}
void cli_case(const fs::path &dir, const fs::path &exe) {
    auto source = dir / "source", backup = dir / "backup", restored = dir / "restored";
    std::string id, hash;
    {
        HistoryStore store(source);
        id = store.admit("main", sample(9));
        hash = digest(store, id);
    }
    cli(exe, source, L"verify");
    cli(exe, source, L"backup", backup);
    cli(exe, backup, L"restore", restored);
    cli(exe, restored, L"verify");
    HistoryStore recovered(restored);
    require(digest(recovered, id) == hash, "Headless recovery lost persistent content");
    std::cout << "verify, backup, restore and verified reopen passed the packaged command interface\n";
}

void capture_freeze(const fs::path& dir) {
    const auto live = dir / "live", backup = dir / "backup";
    std::promise<void> installed, release_capture, pinned, release_backup, binding, collecting;
    auto at_install = installed.get_future(), at_freeze = pinned.get_future(), at_bind = binding.get_future();
    auto at_gc = collecting.get_future();
    auto finish_capture = release_capture.get_future(), finish_backup = release_backup.get_future();
    std::atomic<bool> running{}, reject{};
    domain::Id previous, snip, admitted;
    const auto old_data = sample(2), new_data = sample(42), failed_data = sample(64);
    const auto old_hash = sha256(old_data.formats.front().bytes), new_hash = sha256(new_data.formats.front().bytes);
    const auto failed_hash = sha256(failed_data.formats.front().bytes);
    {
        HistoryStore store(live, [&](std::string_view point) {
            if (reject && point == "transaction") throw std::runtime_error("Injected WAL admission failure");
            if (!running) return;
            if (point == "installed") {
                installed.set_value();
                require(finish_capture.wait_for(8s) == std::future_status::ready, "Capture barrier timed out");
            } else if (point == "backup-pinned") {
                pinned.set_value();
                require(finish_backup.wait_for(8s) == std::future_status::ready, "Backup barrier timed out");
            } else if (point == "binding-original") binding.set_value();
        });
        store.set_capacity("main", 1);
        previous = store.admit("main", old_data);
        auto floating = sample(5, 16);
        floating.formats.front().name = "PNG";
        const auto other = store.create_rope("Snip", 1);
        snip = store.admit(other, floating);
        store.detach(snip);
        const auto snapshot = store.entry(snip).snapshot_id;
        running = true;
        auto capture = std::async(std::launch::async, [&] { return store.admit_capture("main", new_data); });
        require(at_install.wait_for(5s) == std::future_status::ready &&
                fs::is_regular_file(safe_blob_path(live, new_hash)), "Capture did not install the new file before commit");
        auto bind = std::async(std::launch::async, [&] {
            return store.snip_original(snapshot, "C:/fixture/snip.png", {}, true);
        });
        require(at_bind.wait_for(2s) == std::future_status::ready, "Snip writer did not reach the primary connection");
        auto reader = std::async(std::launch::async, [&] { return store.hanging("main"); });
        require(reader.wait_for(1s) == std::future_status::ready && reader.get().front().id == previous &&
                bind.wait_for(0ms) == std::future_status::timeout, "Snip writer blocked or invalidated a primary read");
        auto gc = std::async(std::launch::async, [&] { collecting.set_value(); store.gc(); });
        require(at_gc.wait_for(2s) == std::future_status::ready, "GC worker did not start");
        auto exporter = std::async(std::launch::async, [&] { store.database().backup_to(backup); });
        require(at_freeze.wait_for(1s) == std::future_status::ready, "Backup could not freeze the preceding commit");
        try {
            HistoryStore duplicate(live);
            throw std::runtime_error("Sibling WAL connection released the profile process lock");
        } catch (const StorageFailure& e) { require(e.reason() == StorageProblem::busy, "Unexpected profile lock failure"); }
        release_capture.set_value();
        admitted = capture.get();
        require(bind.get() == "C:/fixture/snip.png", "Snip bind lost after capture committed");
        gc.get();
        // The backup still leases old_data, although the live writer evicted its only entry.
        require(fs::exists(safe_blob_path(live, old_hash)) && digest(store, admitted) == new_hash,
                "GC removed pinned backup bytes or freshly admitted bytes");
        release_backup.set_value();
        exporter.get();
        running = false;
        store.gc();
        require(!fs::exists(safe_blob_path(live, old_hash)), "Old file survived GC after unpin");
        reject = true;
        history_fixture::rejects([&] { store.admit_capture("main", failed_data); }, "Faulted WAL admission committed");
        reject = false;
        store.gc();
        require(order(store, "main") == std::vector{admitted} && !fs::exists(safe_blob_path(live, failed_hash)),
                "Rejected capture evicted a live entry or left a referenced file");
    }
    Database::verify_store(backup);
    HistoryStore frozen(backup), reopened(live);
    require(order(frozen, "main") == std::vector{previous} && digest(frozen, previous) == old_hash &&
            frozen.load(snip).snapshot.screenshot_path.empty() &&
            order(reopened, "main") == std::vector{admitted} && digest(reopened, admitted) == new_hash &&
            reopened.load(snip).snapshot.screenshot_path == "C:/fixture/snip.png",
            "Frozen and live stores did not reopen at their respective commits");
    std::cout << "Capture WAL, concurrent snip, pinned pre-commit backup, GC, rollback and restart verified\n";
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    try {
        history_fixture::Directory dir;
        if (argc == 1)
            run(dir.path);
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--deleted-content")
            deleted_content(dir.path);
        else if (argc == 3 && std::wstring_view(argv[1]) == L"--manifest")
            manifest_case(dir.path, argv[2]);
        else if (argc == 2 && std::wstring_view(argv[1]) == L"--capture-freeze")
            capture_freeze(dir.path);
        else if (argc == 2)
            cli_case(dir.path, argv[1]);
        else
            return 2;
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
