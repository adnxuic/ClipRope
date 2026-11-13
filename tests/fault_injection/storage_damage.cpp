#include "../integration/history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "directory_access.hpp"
#include <chrono>
#include <fstream>
#include <iostream>

namespace {
using namespace cliprope;
using namespace persistence;
using history_fixture::require;
namespace fs = std::filesystem;

domain::Snapshot content(unsigned char marker) {
    domain::Format format;
    format.name = "Damage fixture";
    format.bytes.assign(18000, marker);
    return {{format}};
}
template <class F> void failure(F action, StorageProblem reason) {
    try {
        action();
    } catch (const StorageFailure &e) {
        require(e.reason() == reason, "Unexpected storage failure class");
        const std::string_view text = e.what();
        require(text.find(":\\") == text.npos && text.find("Private-looking") == text.npos,
                "Diagnostic leaked a private path or content");
        require(text.find("retry") != text.npos || text.find("restore") != text.npos ||
                    text.find("Restore") != text.npos,
                "Diagnostic omitted a recovery action");
        return;
    }
    throw std::runtime_error("Expected a storage failure");
}
std::string file_hash(const fs::path &file) {
    std::ifstream in(file, std::ios::binary);
    require(in.good(), "Missing fixture data");
    std::vector<unsigned char> bytes(std::istreambuf_iterator<char>(in), {});
    return sha256(bytes);
}
void run(const fs::path &dir) {
    const auto root = dir / "Private-looking-profile", backup = dir / "backup", restored = dir / "restored";
    std::string first, second;
    const auto hash = sha256(content(1).formats[0].bytes), other = sha256(content(2).formats[0].bytes);
    {
        HistoryStore store(root);
        store.set_capacity("main", 2);
        first = store.admit("main", content(1));
        second = store.admit("main", content(2));
        store.database().backup_to(backup);
        {
            history_fixture::DenyDirectoryAccess denied(root / "staging", FILE_ADD_FILE);
            failure([&] { store.admit("main", content(3)); }, StorageProblem::access);
        }
        auto rope = store.hanging("main");
        require(rope.size() == 2 && rope[0].id == second && rope[1].id == first,
                "Denied admission changed membership or evicted tail");
        require(fs::is_empty(root / "staging"), "Denied write left a partial file");
    }
    require(fs::remove(safe_blob_path(root, hash)), "Could not remove synthetic blob");
    {
        HistoryStore store(root);
        failure([&] { store.load(first); }, StorageProblem::missing_blob);
        require(sha256(store.load(second).snapshot.formats[0].bytes) == other &&
                    store.hanging("main").size() == 2,
                "Missing blob hid or changed another entry");
        failure([&] { store.database().backup_to(dir / "invalid"); }, StorageProblem::missing_blob);
        require(!fs::exists(dir / "invalid"), "Incomplete backup was installed");
    }
    failure([&] { Database::verify_store(root); }, StorageProblem::missing_blob);
    const auto file = root / "history.db";
    {
        std::fstream database(file, std::ios::binary | std::ios::in | std::ios::out);
        require(database.good(), "Cannot open disposable DB");
        database.write("broken header...", 16);
    }
    const auto before = file_hash(file);
    failure([&] { HistoryStore corrupted(root); }, StorageProblem::damaged_database);
    require(file_hash(file) == before && file_hash(safe_blob_path(root, other)) == other,
            "Opening damaged database replaced recoverable bytes");
    Database::restore_backup(backup, restored);
    Database::verify_store(restored);
    HistoryStore recovered(restored);
    require(recovered.hanging("main").size() == 2 &&
                sha256(recovered.load(first).snapshot.formats[0].bytes) == hash &&
                sha256(recovered.load(second).snapshot.formats[0].bytes) == other,
            "Original backup did not recover both entries");
    std::cout << "staging_DACL=access_denied admission_rolled_back=1 missing_blob=diagnosed "
                 "healthy_sibling=readable corrupt_sql=preserved verified_recovery=2\n";
}

void locked_debris(const fs::path& root) {
    std::string hanging, floating;
    {
        HistoryStore seed(root);
        hanging = seed.admit("main", content(1));
        floating = seed.admit("main", content(2));
        seed.detach(floating);
    }
    const auto staging = root / "staging";
    const auto locked = staging / (std::string(32,'a') + ".part");
    const auto disposable = staging / (std::string(32,'b') + ".part");
    const auto unknown = staging / "keep.part";
    for (const auto& file : {locked, disposable, unknown})
        require(std::ofstream(file).put('x').good(), "Could not seed debris");
    auto handle = CreateFileW(locked.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    require(handle != INVALID_HANDLE_VALUE, "Could not hold disposable staging file");
    std::unique_ptr<void, decltype(&CloseHandle)> hold(handle, CloseHandle);
    HistoryStore store(root);
    const auto result = store.startup_collection();
    require(result.removed == 1 && result.deferred == 1 && fs::exists(locked) && !fs::exists(disposable) &&
            fs::exists(unknown), "A locked file stopped or broadened startup reclamation");
    const auto added = store.admit("main", content(3));
    require(store.hanging("main").front().id == added &&
            store.load(hanging).snapshot.formats.front().bytes == content(1).formats.front().bytes &&
            store.load(floating).snapshot.formats.front().bytes == content(2).formats.front().bytes,
            "Deferred cleanup changed reachable Hanging or Floating bytes");
    hold.reset();
    const auto retry = store.gc();
    require(retry.removed == 1 && !retry.deferred && !fs::exists(locked) && fs::exists(unknown),
            "A normal sweep did not reclaim the released debris");
}

void duplicate_blob_and_screenshot(const fs::path& root) {
    const auto sample = content(7);
    const auto& bytes = sample.formats.front().bytes;
    const auto hash = sha256(bytes);
    std::string first, second;
    {
        HistoryStore store(root);
        store.set_capacity("main", 2);
        first = store.admit("main", sample);
        second = store.admit("main", sample);
        const auto blob = safe_blob_path(root, hash);
        const auto before = store.hanging("main");
        auto unchanged = [&] {
            const auto after = store.hanging("main");
            require(after.size() == 2 && after[0].id == before[0].id && after[1].id == before[1].id,
                    "Failed duplicate admission changed the ordered history");
        };
        {
            HANDLE held = CreateFileW(blob.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            require(held != INVALID_HANDLE_VALUE, "Cannot lock disposable blob");
            std::unique_ptr<void, decltype(&CloseHandle)> guard(held, CloseHandle);
            failure([&] { store.admit("main", sample); }, StorageProblem::access);
        }
        unchanged();
        {
            std::fstream change(blob, std::ios::in | std::ios::out | std::ios::binary);
            require(change.put('x').good(), "Cannot change test blob");
        }
        bool mismatch = false;
        try { store.admit("main", sample); }
        catch (const std::runtime_error& e) {
            mismatch = std::string_view(e.what()).find("existing blob failed verification") != std::string_view::npos;
        }
        require(mismatch, "Expected the existing immutable Blob verification error");
        unchanged();
        fs::resize_file(blob, bytes.size() - 1);
        failure([&] { store.admit("main", sample); }, StorageProblem::damaged_blob);
        unchanged();
        {
            std::ofstream restore(blob, std::ios::binary);
            require(restore.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()).good(),
                    "Cannot restore fixture blob");
        }
        const auto entry = store.entry(first);
        const auto file = store.materialize_screenshot(entry, bytes);
        const auto time = fs::last_write_time(file) - std::chrono::hours(1);
        fs::last_write_time(file, time);
        require(store.materialize_screenshot(entry, bytes) == file && fs::last_write_time(file) == time,
                "Identical managed screenshot was replaced");
        {
            HANDLE held = CreateFileW(file.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            require(held != INVALID_HANDLE_VALUE, "Cannot lock managed test screenshot");
            std::unique_ptr<void, decltype(&CloseHandle)> guard(held, CloseHandle);
            failure([&] { store.materialize_screenshot(entry, bytes); }, StorageProblem::access);
        }
        require(fs::last_write_time(file) == time, "Unreadable managed screenshot was replaced");
        {
            std::fstream change(file, std::ios::in | std::ios::out | std::ios::binary);
            require(change.put('y').good(), "Cannot change managed test screenshot");
        }
        require(file_sha256(store.materialize_screenshot(entry, bytes)) == hash, "Damaged screenshot not repaired");
        fs::resize_file(file, 4);
        require(file_sha256(store.materialize_screenshot(entry, bytes)) == hash, "Short screenshot not repaired");
    }
    HistoryStore reopened(root);
    const auto order = reopened.hanging("main");
    require(order.size() == 2 && order[0].id == second && order[1].id == first &&
                reopened.load(first).snapshot.formats.front().bytes == bytes,
            "Duplicate verification failed after restart");
}
} // namespace
int main() {
    try {
        history_fixture::Directory tmp;
        run(tmp.path);
        locked_debris(tmp.path / "locked-debris");
        duplicate_blob_and_screenshot(tmp.path / "duplicate-blob");
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
