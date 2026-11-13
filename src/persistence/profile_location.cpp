#include "persistence/profile_location.hpp"
#include <windows.h>
#include <algorithm>
#include <map>
#include <array>
#include <fstream>
#include <set>

namespace cliprope::persistence {
namespace fs = std::filesystem;
namespace {
using Handle = std::unique_ptr<void, decltype(&CloseHandle)>;
using Manifest = std::map<fs::path, std::string>;
void require(bool ok, const char* message) {
    if (!ok) throw LocationError(message);
}
void safe_chain(const fs::path& path) {
    require(path.is_absolute() && path.lexically_normal() == path && path.root_name().wstring().size() == 2,
            "Choose an ordinary local folder.");
    for (auto p = path; p != p.root_path(); p = p.parent_path()) {
        const auto name = p.filename().wstring();
        require(!name.empty() && name.find_first_of(L":*?") == name.npos && name.back() != L'.' && name.back() != L' ',
                "Invalid folder or file name.");
        const auto flags = GetFileAttributesW(p.c_str());
        if (flags == INVALID_FILE_ATTRIBUTES) {
            const auto e = GetLastError();
            require(e == ERROR_PATH_NOT_FOUND || e == ERROR_FILE_NOT_FOUND, "Cannot inspect the data path.");
        } else
            require(!(flags & FILE_ATTRIBUTE_REPARSE_POINT), "Linked folders and files cannot be used.");
    }
}
Handle open_handle(const fs::path& path, DWORD access, DWORD share, DWORD disposition, DWORD flags) {
    auto h =
        CreateFileW(path.c_str(), access, share, nullptr, disposition, flags | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw StorageFailure(GetLastError() == ERROR_SHARING_VIOLATION ? StorageProblem::busy : StorageProblem::access);
    return Handle(h, CloseHandle);
}
std::string identity(const fs::path& dir) {
    safe_chain(dir);
    auto h = open_handle(dir, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(h.get(), &info) && !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT),
            "Cannot identify data folder.");
    return std::to_string(info.dwVolumeSerialNumber) + "-" + std::to_string(info.nFileIndexHigh) + "-" +
           std::to_string(info.nFileIndexLow);
}
Handle profile_lock(const fs::path& root, DWORD disposition, bool temporary = false) {
    safe_chain(root / ".writer.lock");
    return open_handle(root / ".writer.lock", GENERIC_READ | GENERIC_WRITE | DELETE, 0, disposition,
                       FILE_ATTRIBUTE_HIDDEN | (temporary ? FILE_FLAG_DELETE_ON_CLOSE : 0));
}
void retire_lock(Handle& lock) {
    FILE_DISPOSITION_INFO info{TRUE};
    require(SetFileInformationByHandle(lock.get(), FileDispositionInfo, &info, sizeof(info)),
            "Cannot release old lock file.");
    lock.reset();
}
bool within(const fs::path& path, const fs::path& ancestor) {
    for (auto p = path;; p = p.parent_path()) {
        if (fs::exists(p) && fs::exists(ancestor) && fs::equivalent(p, ancestor)) return true;
        if (p == p.root_path()) return false;
    }
}
bool hex(std::wstring_view text, size_t count) {
    return text.size() == count && text.find_first_not_of(L"0123456789abcdef") == text.npos;
}
bool owned_file(const fs::path& relative) {
    const auto parent = relative.parent_path();
    auto name = relative.filename().wstring();
    if (parent.empty())
        return name == L"history.db" || name == L"history.db-wal" || name == L"history.db-shm" ||
               name == L"history.db-journal" || name == L"diagnostics.txt";
    if (parent == L"screenshots") return relative.extension() == L".png" && hex(relative.stem().wstring(), 32);
    if (parent == L"staging") return relative.extension() == L".part" && hex(relative.stem().wstring(), 32);
    if (parent == L"thumbnails") {
        if (name.ends_with(L".tmp")) name.resize(name.size() - 4);
        return name.size() > 68 && hex(std::wstring_view(name).substr(0, 64), 64) && name[64] == L'-' &&
               name.ends_with(L".thumb");
    }
    if (parent.parent_path() == L"blobs")
        return relative.extension() == L".blob" && hex(relative.stem().wstring(), 64) &&
               parent.filename() == name.substr(0, 2);
    if (*relative.begin() == L"backups") {
        const auto rest = relative.lexically_relative(L"backups");
        const auto sub = rest.lexically_relative(*rest.begin());
        return sub == L"history.db" || (!sub.empty() && *sub.begin() == L"blobs" && owned_file(sub));
    }
    return false;
}
bool owned_folder(const fs::path& rel) {
    if (rel == L"backups" || rel == L"blobs" || rel == L"thumbnails" || rel == L"screenshots" || rel == L"staging")
        return true;
    if (rel.parent_path() == L"blobs") return hex(rel.filename().wstring(), 2);
    if (*rel.begin() != L"backups") return false;
    const auto rest = rel.lexically_relative(L"backups");
    if (!rest.has_parent_path()) return true;
    const auto sub = rest.lexically_relative(*rest.begin());
    return sub == L"blobs" || (sub.parent_path() == L"blobs" && hex(sub.filename().wstring(), 2));
}
bool recognized_history(const fs::path& folder) {
    const auto path = folder / "history.db";
    safe_chain(path);
    if (!fs::is_regular_file(path)) return false;
    try { require_history_header(path); }
    catch (const StorageFailure& failure) {
        if (failure.reason() == StorageProblem::damaged_database ||
            failure.reason() == StorageProblem::unsupported_format) return false;
        throw;
    }
    return true;
}
std::set<fs::path> screenshot_files(const fs::path& root) {
    std::set<fs::path> result;
    if (!fs::is_directory(root / "screenshots")) return result;
    const auto file = root / "history.db";
    safe_chain(file);
    sqlite3* raw{};
    const auto name = file.u8string();
    check_closed_history(root);
    const auto rc = sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &raw, SQLITE_OPEN_READONLY, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    require(rc == SQLITE_OK, "Cannot read the screenshot inventory.");
    require_current_history(raw);
    Statement files(raw, "SELECT snapshot_id FROM managed_screenshots");
    while (files.step())
        result.emplace(files.wide(0) + L".png");
    return result;
}
template <class Visit> void inventory(const fs::path& root, const std::set<fs::path>& screenshots, Visit visit) {
    safe_chain(root);
    for (fs::recursive_directory_iterator it(root), end; it != end; ++it) {
        const auto rel = it->path().lexically_relative(root);
        const auto flags = GetFileAttributesW(it->path().c_str());
        if (flags == INVALID_FILE_ATTRIBUTES) throw StorageFailure(StorageProblem::access);
        if (flags & FILE_ATTRIBUTE_REPARSE_POINT) {
            it.disable_recursion_pending();
            require(!owned_folder(rel) && !owned_file(rel), "Managed data contains a link.");
        } else if (flags & FILE_ATTRIBUTE_DIRECTORY) {
            if (!owned_folder(rel) || (rel.parent_path() == L"backups" && !recognized_history(it->path())))
                it.disable_recursion_pending();
        } else if (owned_file(rel) && (rel.parent_path() != L"screenshots" || screenshots.contains(rel.filename())))
            visit(rel);
    }
}
void remove_empty_parents(fs::path dir, const fs::path& root) {
    while (dir != root && RemoveDirectoryW(dir.c_str()))
        dir = dir.parent_path();
}
void remove_listed(const fs::path& root, const Manifest& files, bool partial) {
    for (const auto& [rel, hash] : files) {
        const auto path = root / rel;
        safe_chain(path);
        if (!fs::exists(path)) {
            remove_empty_parents(path.parent_path(), root);
            continue;
        }
        if (partial)
            fs::remove(path);
        else
            remove_verified_file(path, hash);
        remove_empty_parents(path.parent_path(), root);
    }
}
std::string scalar_string(sqlite3* db, const char* sql) {
    Statement s(db, sql);
    require(s.step(), "Profile location is unreadable.");
    return s.text(0);
}
} // namespace

struct ProfileLocation::State {
    fs::path dir, default_root;
    Handle guard{nullptr, CloseHandle};
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection{nullptr, sqlite3_close};
    sqlite3* db() const { return connection.get(); }
    fs::path path(const char* sql) const {
        Statement s(db(), sql);
        require(s.step(), "Missing profile location.");
        return s.wide(0);
    }
    Manifest files(std::string_view token) const {
        Manifest result;
        Statement s(db(), "SELECT path,digest FROM files WHERE token=?");
        s.bind(1, token);
        while (s.step()) {
            fs::path rel = s.wide(0);
            require(!rel.has_root_path() && rel.lexically_normal() == rel && owned_file(rel),
                    "Invalid migration manifest.");
            result.emplace(rel, s.text(1));
        }
        return result;
    }
    void clear_transfer(std::string_view token) {
        Statement remove(db(), "DELETE FROM transfers WHERE token=?");
        remove.bind(1, token);
        remove.run();
        Statement done(db(), "UPDATE location SET previous='',source_id='',token='' WHERE token=?");
        done.bind(1, token);
        done.run();
    }
    // The staging directory is private to this attempt. Published files must
    // still match their pre-cutover digest before a failed attempt can be undone.
    void undo(const std::string& token) {
        if (token.empty()) return;
        require(hex(fs::path(token).wstring(), 32), "Invalid staging identifier.");
        Statement transfer(db(), "SELECT target,target_id FROM transfers WHERE token=?");
        transfer.bind(1, token);
        require(transfer.step(), "Missing migration record.");
        const fs::path target(transfer.wide(0));
        require(identity(target) == transfer.text(1), "The destination folder has changed.");
        const bool new_lock = !fs::exists(target / ".writer.lock");
        auto lock = profile_lock(target, new_lock ? CREATE_NEW : OPEN_EXISTING, new_lock);
        const auto stage = target / (L".cliprope-" + fs::path(token).wstring());
        safe_chain(stage);
        const auto manifest = files(token);
        Manifest installed;
        Statement published(db(), "SELECT path,digest,file_id FROM files WHERE token=? AND file_id IS NOT NULL");
        published.bind(1, token);
        while (published.step()) {
            fs::path rel = published.wide(0);
            if (!manifest.contains(rel)) throw std::runtime_error("Invalid published file.");
            safe_chain(target / rel);
            if (fs::exists(target / rel) && identity(target / rel) == published.text(2))
                installed.emplace(rel, published.text(1));
            else if (!fs::exists(target / rel))
                remove_empty_parents((target / rel).parent_path(), target);
        }
        // A process can stop while SQLite is verifying its copy. Close its WAL
        // before removing published files, and retain a changed destination DB.
        auto close_wal = [](const fs::path& folder) {
            for (auto name : {"history.db", "history.db-shm", "history.db-wal", "history.db-journal"})
                safe_chain(folder / name);
            if (fs::exists(folder / "history.db") &&
                (fs::exists(folder / "history.db-shm") || fs::exists(folder / "history.db-wal") ||
                 fs::exists(folder / "history.db-journal")))
                Database::checkpoint_existing(folder);
        };
        if (installed.contains(L"history.db")) close_wal(target);
        close_wal(stage);
        remove_listed(target, installed, false);
        remove_listed(stage, manifest, true);
        if (fs::exists(stage)) require(fs::remove(stage), "Cannot remove migration staging.");
        Transaction tx(db());
        clear_transfer(token);
        tx.commit();
    }
    void clean_source(const fs::path& root, Handle& lock) {
        require(identity(root) == scalar_string(db(), "SELECT source_id FROM location"),
                "The previous folder has changed.");
        const auto token = scalar_string(db(), "SELECT token FROM location");
        const auto manifest = files(token);
        // Check the database first, even after an interrupted cleanup. A new
        // writer may have explicitly reopened the old directory in the meantime.
        if (fs::exists(root / "history.db"))
            require(file_sha256(root / "history.db") == manifest.at(L"history.db") &&
                        !fs::exists(root / "history.db-wal"),
                    "Previous history changed and was preserved.");
        remove_listed(root, manifest, false);
        // Cache and staging directories can already be empty when migration starts.
        for (auto name : {"staging", "thumbnails", "screenshots", "backups", "blobs"})
            RemoveDirectoryW((root / name).c_str());
        retire_lock(lock);
        Transaction tx(db());
        clear_transfer(token);
        tx.commit();
    }
};

ProfileLocation::ProfileLocation(const fs::path& local) : state_(std::make_unique<State>()) {
    auto& s = *state_;
    s.dir = local / "ClipRope.Settings";
    s.default_root = local / "ClipRope";
    safe_chain(s.dir);
    fs::create_directories(s.dir);
    safe_chain(s.dir / "session.lock");
    s.guard = open_handle(s.dir / "session.lock", GENERIC_WRITE | DELETE, 0, OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN);
    for (auto file : {"location.db", "location.db-journal", "location.db-wal", "location.db-shm"})
        safe_chain(s.dir / file);
    const auto name = (s.dir / "location.db").u8string();
    sqlite3* raw{};
    const auto rc = sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &raw,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    s.connection.reset(raw);
    require(rc == SQLITE_OK, "Cannot open profile location settings.");
    exec(raw,
         "PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON; CREATE TABLE IF NOT EXISTS location(id INTEGER PRIMARY KEY "
         "CHECK(id=1),"
         "active TEXT,pending TEXT,previous TEXT,source_id TEXT,target_id TEXT,token TEXT,active_id TEXT);"
         "INSERT OR IGNORE INTO location VALUES(1,'','','','','','','');"
         "CREATE TABLE IF NOT EXISTS transfers(token TEXT PRIMARY KEY,target TEXT NOT NULL,target_id TEXT NOT NULL);"
         "CREATE TABLE IF NOT EXISTS files(token TEXT REFERENCES transfers(token) ON DELETE CASCADE,"
         "path TEXT,digest TEXT NOT NULL,file_id TEXT,PRIMARY KEY(token,path))");
}
ProfileLocation::~ProfileLocation() = default;
std::vector<fs::path> ProfileLocation::reserved_directories(const fs::path& local) {
    const auto settings = local / L"ClipRope.Settings";
    std::vector<fs::path> roots{local / L"ClipRope", settings};
    const auto locator = settings / L"location.db";
    safe_chain(locator);
    if (!fs::exists(locator)) return roots;
    for (auto file : {L"location.db-journal", L"location.db-wal", L"location.db-shm"})
        safe_chain(settings / file);
    const auto name = locator.u8string();
    sqlite3* raw{};
    const auto rc = sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &raw, SQLITE_OPEN_READONLY, nullptr);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> connection(raw, sqlite3_close);
    require(rc == SQLITE_OK, "Cannot read profile location settings.");
    Statement paths(raw, "SELECT active FROM location UNION SELECT pending FROM location "
                         "UNION SELECT previous FROM location UNION SELECT target FROM transfers");
    while (paths.step()) {
        const fs::path path(paths.wide(0));
        if (!path.empty()) {
            require(path.is_absolute(), "Invalid stored profile folder.");
            roots.push_back(path);
        }
    }
    return roots;
}
fs::path ProfileLocation::active() const {
    const auto path = state_->path("SELECT active FROM location");
    if (path.empty()) return state_->default_root;
    require(identity(path) == scalar_string(state_->db(), "SELECT active_id FROM location"),
            "Configured history folder has changed.");
    safe_chain(path / "history.db");
    require(fs::is_regular_file(path / "history.db"), "The configured history folder is unavailable.");
    return path;
}
fs::path ProfileLocation::pending() const { return state_->path("SELECT pending FROM location"); }

void ProfileLocation::schedule(const fs::path& target) {
    auto& s = *state_;
    require(s.path("SELECT previous FROM location").empty(), "Previous history cleanup is still pending.");
    if (target == pending()) return;
    std::string id;
    if (!target.empty()) {
        safe_chain(target);
        auto drive = GetDriveTypeW(target.root_path().c_str());
        require((drive == DRIVE_FIXED || drive == DRIVE_REMOVABLE || drive == DRIVE_RAMDISK) &&
                    target != target.root_path() && fs::is_directory(target) && fs::is_empty(target),
                "Choose an empty local data folder.");
        const auto source = active();
        require(!within(target, source) && !within(source, target) && !within(target, s.dir) && !within(s.dir, target),
                "Choose a folder separate from the current history and settings.");
        id = identity(target);
        auto probe = open_handle(target / ".cliprope-write-probe", GENERIC_WRITE | DELETE, 0, CREATE_NEW,
                                 FILE_FLAG_DELETE_ON_CLOSE);
    }
    const auto token = scalar_string(s.db(), "SELECT token FROM location");
    try {
        s.undo(token);
    } catch (const std::exception&) {
        // An unplugged or busy abandoned destination retains its cleanup receipt.
        // Scheduling another destination must not make the live source unusable.
    }
    const auto current = active();
    Statement write(s.db(), "UPDATE location SET pending=?,target_id=?,active=?,active_id=?,token='',source_id=''");
    write.bind(1, target.wstring());
    write.bind(2, id);
    write.bind(3, current.wstring());
    write.bind(4, identity(current));
    write.run();
}
void ProfileLocation::resume(const Database::Fault& fault) {
    auto& s = *state_;
    std::vector<std::string> abandoned;
    {
        Statement list(s.db(), "SELECT token FROM transfers WHERE token<>(SELECT token FROM location)");
        while (list.step())
            abandoned.push_back(list.text(0));
    }
    bool incomplete = false;
    for (const auto& token : abandoned) {
        try {
            s.undo(token);
        } catch (const std::exception&) {
            incomplete = true;
        }
    }
    auto old = s.path("SELECT previous FROM location");
    if (!old.empty()) {
        Database::verify_store(active());
        auto lock = profile_lock(old, OPEN_ALWAYS);
        s.clean_source(old, lock);
    }
    const auto target = pending();
    if (target.empty()) {
        require(!incomplete, "A previously selected data folder is unavailable for cleanup.");
        return;
    }
    const auto source = active();
    require(identity(target) == scalar_string(s.db(), "SELECT target_id FROM location"),
            "Destination folder was replaced.");
    s.undo(scalar_string(s.db(), "SELECT token FROM location"));
    require(fs::is_empty(target) && !within(target, source) && !within(source, target),
            "Destination is no longer empty or separate.");
    auto source_guard = profile_lock(source, OPEN_EXISTING);
    for (const auto file : {"history.db", "history.db-journal", "history.db-wal", "history.db-shm"})
        safe_chain(source / file);
    Database::verify_store(source);
    const auto screenshots = screenshot_files(source);
    Database::checkpoint_existing(source);
    Manifest manifest;
    inventory(source, screenshots, [&](const auto& rel) { manifest.emplace(rel, file_sha256(source / rel)); });
    const auto token = scalar_string(s.db(), "SELECT lower(hex(randomblob(16)))");
    const auto stage = target / (".cliprope-" + token);
    {
        Transaction tx(s.db());
        Statement update(s.db(), "UPDATE location SET source_id=?,token=?");
        update.bind(1, identity(source));
        update.bind(2, token);
        update.run();
        Statement record(s.db(), "INSERT INTO transfers SELECT token,pending,target_id FROM location");
        record.run();
        for (const auto& [rel, hash] : manifest) {
            Statement entry(s.db(), "INSERT INTO files(token,path,digest) VALUES(?,?,?)");
            entry.bind(1, token);
            entry.bind(2, rel.wstring());
            entry.bind(3, hash);
            entry.run();
        }
        tx.commit();
    }
    auto target_guard = profile_lock(target, CREATE_NEW, true);
    require(std::distance(fs::directory_iterator(target), fs::directory_iterator{}) == 1,
            "Destination is no longer empty.");
    require(fs::create_directory(stage), "Cannot create migration staging.");
    for (const auto& [rel, hash] : manifest) {
        safe_chain(stage / rel);
        fs::create_directories((stage / rel).parent_path());
        if (!CopyFileW((source / rel).c_str(), (stage / rel).c_str(), TRUE))
            throw StorageFailure(GetLastError() == ERROR_DISK_FULL ? StorageProblem::full : StorageProblem::access);
        auto copy = open_handle(stage / rel, GENERIC_WRITE, FILE_SHARE_READ, OPEN_EXISTING, 0);
        require(FlushFileBuffers(copy.get()), "Cannot flush copied data.");
        copy.reset();
        require(file_sha256(stage / rel) == hash, "Copied data did not pass verification.");
        if (fault) fault("copy");
    }
    Database::verify_store(stage);
    Database::checkpoint_existing(stage);
    for (const auto& [rel, hash] : manifest) {
        safe_chain(target / rel);
        fs::create_directories((target / rel).parent_path());
        // A same-volume rename preserves this file ID. Record it before the
        // rename so recovery can distinguish our file from a competing one.
        Statement publish(s.db(), "UPDATE files SET file_id=? WHERE path=? AND token=?");
        publish.bind(1, identity(stage / rel));
        publish.bind(2, rel.wstring());
        publish.bind(3, token);
        publish.run();
        require(MoveFileExW((stage / rel).c_str(), (target / rel).c_str(), MOVEFILE_WRITE_THROUGH),
                "Cannot install copied data.");
        remove_empty_parents((stage / rel).parent_path(), stage);
        if (fault) fault("install");
    }
    require(fs::remove(stage), "Cannot close migration staging.");
    Database::verify_store(target);
    Database::checkpoint_existing(target);
    for (const auto& [rel, hash] : manifest)
        require(file_sha256(target / rel) == hash, "Installed files did not pass verification.");
    {
        Statement switch_root(s.db(), "UPDATE location SET previous=?,active=pending,active_id=target_id,pending=''");
        switch_root.bind(1, source.wstring());
        switch_root.run();
    }
    if (fault) fault("cleanup");
    s.clean_source(source, source_guard);
    require(!incomplete, "A previously selected data folder is unavailable for cleanup.");
}

std::uint64_t profile_disk_usage(const fs::path& root) {
    std::uint64_t total{};
    inventory(root, screenshot_files(root), [&](const auto& rel) {
        auto h = open_handle(root / rel, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             OPEN_EXISTING, 0);
        FILE_STANDARD_INFO info{};
        require(GetFileInformationByHandleEx(h.get(), FileStandardInfo, &info, sizeof(info)),
                "Cannot read allocated file size.");
        total += info.AllocationSize.QuadPart;
    });
    return total;
}

void ProfileLocation::purge() {
    auto& s = *state_;
    if (!pending().empty()) schedule({});
    resume();
    const auto root = active();
    safe_chain(root);
    if (fs::exists(root) && !fs::is_empty(root)) {
        auto guard = profile_lock(root, OPEN_EXISTING);
        require(recognized_history(root), "Refusing to erase an unrecognized history database.");
        Manifest files;
        inventory(root, screenshot_files(root), [&](const auto& rel) { files.emplace(rel, file_sha256(root / rel)); });
        // Preserve the uninstaller's refusal of unrecognized files.
        for (const auto& item : fs::recursive_directory_iterator(root)) {
            safe_chain(item.path());
            const auto rel = item.path().lexically_relative(root);
            require(item.is_directory() ? owned_folder(rel) : (rel == L".writer.lock" || files.contains(rel)),
                    "The profile contains unrecognized files.");
            if (rel != L".writer.lock") {
                require(item.is_directory() || !(GetFileAttributesW(item.path().c_str()) & FILE_ATTRIBUTE_READONLY),
                        "The profile contains read-only files.");
                auto check = open_handle(item.path(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
            }
        }
        const auto database = files.extract(L"history.db");
        require(!database.empty(), "Missing history database.");
        remove_listed(root, files, false);
        remove_verified_file(root / database.key(), database.mapped());
        retire_lock(guard);
        // GC can leave empty shards and cache directories.
        std::set<fs::path> dirs;
        for (const auto& item : fs::recursive_directory_iterator(root))
            if (item.is_directory()) dirs.insert(item.path());
        for (auto it = dirs.rbegin(); it != dirs.rend(); ++it)
            fs::remove(*it);
    }
    if (fs::exists(root)) fs::remove(root);
    s.connection.reset();
    fs::remove(s.dir / "location.db");
    retire_lock(s.guard);
    RemoveDirectoryW(s.dir.c_str()); // Unrelated files keep the directory in place.
}
} // namespace cliprope::persistence
