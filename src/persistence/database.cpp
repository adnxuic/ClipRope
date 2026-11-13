#include "persistence/database.hpp"
#include "persistence/file_digest.hpp"
#include "persistence/manifest.hpp"
#include "database_schema.hpp"

#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <fstream>
#include <format>
#include <limits>
#include <memory>
#include <stdexcept>

namespace cliprope::persistence {
namespace fs = std::filesystem;
namespace {
constexpr int writer_timeout_ms = 1500;

void check(int result) {
    switch (result & 0xff) {
    case SQLITE_BUSY: case SQLITE_LOCKED: throw StorageFailure(StorageProblem::busy);
    case SQLITE_FULL: throw StorageFailure(StorageProblem::full);
    case SQLITE_CORRUPT: case SQLITE_NOTADB: throw StorageFailure(StorageProblem::damaged_database);
    case SQLITE_PERM: case SQLITE_AUTH: case SQLITE_READONLY: case SQLITE_CANTOPEN:
        throw StorageFailure(StorageProblem::access);
    }
    if (result != SQLITE_OK)
        throw std::runtime_error(std::string("History database: ") + sqlite3_errstr(result));
}

void checked_hash(NTSTATUS status) {
    if (status < 0) throw std::runtime_error("Could not hash history bytes");
}

class Hash {
public:
    Hash() {
        checked_hash(BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0));
        const auto status = BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0);
        if (status < 0) { BCryptCloseAlgorithmProvider(algorithm_, 0); checked_hash(status); }
    }
    ~Hash() { BCryptDestroyHash(hash_); BCryptCloseAlgorithmProvider(algorithm_, 0); }
    Hash(const Hash&) = delete;
    Hash& operator=(const Hash&) = delete;
    void append(std::span<const unsigned char> bytes) {
        while (!bytes.empty()) {
            auto n = static_cast<ULONG>(std::min<size_t>(bytes.size(), std::numeric_limits<ULONG>::max()));
            checked_hash(BCryptHashData(hash_, const_cast<PUCHAR>(bytes.data()), n, 0));
            bytes = bytes.subspan(n);
        }
    }
    std::string finish() {
        std::array<UCHAR,32> digest{};
        checked_hash(BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0));
        constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
        return result;
    }
private:
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
};

void refuse_reparse(const fs::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const auto e = GetLastError();
        if (e != ERROR_FILE_NOT_FOUND && e != ERROR_PATH_NOT_FOUND)
            throw std::runtime_error("History path cannot be inspected");
    } else if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) throw std::runtime_error("History path contains a reparse point");
}

sqlite3* open_sql(const fs::path& file,int flags=SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE) {
    sqlite3* db{};
    const auto path = file.u8string();
    const int rc = sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &db,
                                  flags | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) { sqlite3_close(db); check(rc); }
    return db;
}

std::int64_t scalar(sqlite3* db, const char* sql) {
    Statement query(db, sql);
    if (!query.step()) throw std::runtime_error("History database value missing");
    return query.integer(0);
}

std::string unique_suffix() {
    static std::atomic<unsigned> serial{};
    return std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++serial);
}

void check_blob_copy(const fs::path& file, std::int64_t size, const std::string& hash) {
    std::error_code ec;
    if (size < 0 || fs::file_size(file, ec) != static_cast<std::uintmax_t>(size) || ec)
        throw StorageFailure(StorageProblem::damaged_blob);
    if (file_sha256(file) != hash) throw StorageFailure(StorageProblem::damaged_blob);
}

std::string digest_stream(std::ifstream& in) {
    Hash digest;
    std::vector<unsigned char> chunk(64 * 1024);
    while (in) {
        in.read(reinterpret_cast<char*>(chunk.data()), chunk.size());
        digest.append({chunk.data(), static_cast<size_t>(in.gcount())});
    }
    if (!in.eof()) throw StorageFailure(StorageProblem::access);
    return digest.finish();
}

void verify_foreign_keys(sqlite3* db) {
    Statement check_fk(db, "PRAGMA foreign_key_check");
    if (check_fk.step()) throw StorageFailure(StorageProblem::damaged_database);
}

void verify_rebin(sqlite3* db) {
    if (scalar(db, "SELECT count(*) FROM ropes WHERE id='system:rebin' AND name='ReBin' AND capacity IS NULL AND is_main=0") != 1 ||
         scalar(db, "SELECT count(*) FROM app_settings WHERE active_rope_id='system:rebin'") != 0)
        throw StorageFailure(StorageProblem::damaged_database);
}

void verify_metadata(sqlite3* db) {
    verify_rebin(db);
    if (scalar(db, "SELECT count(*) FROM ropes WHERE is_main=1") != 1 ||
        scalar(db, "SELECT count(*) FROM app_settings a JOIN ropes r ON r.id=a.active_rope_id WHERE a.id=1") != 1 ||
        scalar(db, "SELECT coalesce(max(version),0) FROM schema_migrations") != scalar(db, "PRAGMA user_version"))
        throw StorageFailure(StorageProblem::damaged_database);
}

void create_history(const fs::path& root, const Database::Fault& fault) {
    const auto staging = root / "staging";
    refuse_reparse(staging);
    fs::create_directory(staging);
    std::array<std::uint64_t, 2> random{};
    sqlite3_randomness(sizeof random, random.data());
    const auto path = staging / std::format("{:016x}{:016x}.part", random[0], random[1]);
    const auto file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw StorageFailure(StorageProblem::access);
    CloseHandle(file);
    try {
        {
            std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(open_sql(path, SQLITE_OPEN_READWRITE), sqlite3_close);
            // Only the renamed, committed file can become the history on restart.
            exec(db.get(), "PRAGMA journal_mode=MEMORY; PRAGMA synchronous=FULL; PRAGMA foreign_keys=ON");
            Transaction tx(db.get());
            exec(db.get(), schema::v11);
            require_current_history(db.get());
            verify_foreign_keys(db.get());
            verify_metadata(db.get());
            if (fault) fault("schema-create");
            tx.commit();
        }
        if (!MoveFileExW(path.c_str(), (root / "history.db").c_str(), MOVEFILE_WRITE_THROUGH))
            throw StorageFailure(StorageProblem::access);
    } catch (...) {
        std::error_code ignored;
        fs::remove(path, ignored);
        throw;
    }
}

void install_backup(const fs::path& staging, const fs::path& target) {
    // A Windows reader can briefly open the staged database without share-delete.
    // Only retry the atomic rename. Keep the verified files and never replace a destination.
    const auto deadline = GetTickCount64() + 1000;
    for (;;) {
        if (MoveFileExW(staging.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) return;
        const DWORD e = GetLastError();
        if ((e != ERROR_ACCESS_DENIED && e != ERROR_SHARING_VIOLATION && e != ERROR_LOCK_VIOLATION) ||
            fs::exists(target) || GetTickCount64() >= deadline)
            throw std::runtime_error("Could not install completed backup (Windows error " + std::to_string(e) + ")");
        Sleep(20);
    }
}

} // namespace

StorageFailure::StorageFailure(StorageProblem reason) : HistoryFailure(
    reason == StorageProblem::busy ? application::HistoryIssue::storage_busy : application::HistoryIssue::storage_failed, [&]{
    switch(reason) {
    case StorageProblem::unsupported_format: return "Unsupported history database version. This build requires a ClipRope v11 database.";
    case StorageProblem::access: return "History access denied. Check profile permissions and retry from your Windows account.";
    case StorageProblem::busy: return "History is in use. Fully exit the active ClipRope instance, then retry.";
    case StorageProblem::full: return "History database or disk is full. Free space on the profile drive, then retry.";
    case StorageProblem::missing_blob: return "Damaged content: external blob is missing. Keep the profile and restore a verified backup to a new folder.";
    case StorageProblem::damaged_blob: return "Damaged content: stored bytes failed verification. Preserve the profile and restore a verified backup to a new folder.";
    case StorageProblem::damaged_database: return "History database is damaged. Preserve the entire profile, including WAL and blobs. Restore a verified backup to a new folder.";
    case StorageProblem::damaged_manifest: return "Damaged content: representation manifest failed verification. Preserve the profile and restore a verified backup to a new folder.";
    }
    return "History storage failed.";
}()), reason_(reason) {}

void exec(sqlite3* db, const char* sql) { check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr)); }

std::string sha256(std::span<const unsigned char> bytes) {
    Hash digest;
    digest.append(bytes);
    return digest.finish();
}

std::string file_sha256(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw StorageFailure(StorageProblem::access);
    return digest_stream(in);
}

std::string file_sha256(HANDLE file) {
    std::vector<unsigned char> chunk(64 * 1024);
    Hash digest;
    DWORD count{};
    do {
        if (!ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &count, nullptr))
            throw StorageFailure(StorageProblem::access);
        digest.append({chunk.data(), count});
    } while (count);
    return digest.finish();
}

void remove_verified_file(const fs::path& path, std::string_view expected) {
    // Hold the same file against replacement and writing from hash through deletion.
    const auto handle = CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw StorageFailure(StorageProblem::access);
    std::unique_ptr<void, decltype(&CloseHandle)> guard(handle, CloseHandle);
    FILE_ATTRIBUTE_TAG_INFO attributes{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
        (attributes.FileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        throw StorageFailure(StorageProblem::access);
    if (file_sha256(handle) != expected) throw std::runtime_error("A file changed after verification. It was preserved.");
    FILE_DISPOSITION_INFO erase{TRUE};
    if (!SetFileInformationByHandle(handle, FileDispositionInfo, &erase, sizeof(erase))) throw StorageFailure(StorageProblem::access);
}

void require_current_history(sqlite3* db) {
    if (scalar(db, "PRAGMA application_id") != history_application_id ||
        scalar(db, "PRAGMA user_version") != history_version)
        throw StorageFailure(StorageProblem::unsupported_format);
}

// A closed WAL database can acquire new -wal and -shm files on its first SQLite
// read. Inspect the header directly when there is no pending journal to replay.
void require_history_header(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input) throw StorageFailure(StorageProblem::access);
    std::array<unsigned char, 72> header{};
    if (!input.read(reinterpret_cast<char*>(header.data()), header.size()) ||
        std::string_view(reinterpret_cast<const char*>(header.data()), 16) != std::string_view("SQLite format 3", 16))
        throw StorageFailure(StorageProblem::damaged_database);
    const auto word = [&](size_t offset) {
        return (std::uint32_t(header[offset]) << 24) | (std::uint32_t(header[offset + 1]) << 16) |
               (std::uint32_t(header[offset + 2]) << 8) | header[offset + 3];
    };
    if (word(68) != history_application_id || word(60) != history_version)
        throw StorageFailure(StorageProblem::unsupported_format);
}

bool check_closed_history(const fs::path& root) {
    for (const auto* name : {"history.db", "history.db-wal", "history.db-shm", "history.db-journal"})
        refuse_reparse(root / name);
    for (const auto* name : {"history.db-wal", "history.db-journal"}) {
        const auto journal = root / name;
        if (fs::exists(journal) && fs::file_size(journal) > 0) return false;
    }
    require_history_header(root / "history.db");
    return true;
}

namespace {
void preflight_history(const fs::path& root) {
    if (check_closed_history(root)) return;
    // Committed WAL frames can contain a newer page one than the main file.
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(open_sql(root / "history.db", SQLITE_OPEN_READONLY), sqlite3_close);
    require_current_history(db.get());
}
}

void Database::checkpoint_existing(const fs::path& root) {
    preflight_history(root);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(open_sql(root / "history.db", SQLITE_OPEN_READWRITE), sqlite3_close);
    if (sqlite3_db_readonly(db.get(), "main") != 0) throw StorageFailure(StorageProblem::access);
    require_current_history(db.get());
    if (scalar(db.get(), "PRAGMA wal_checkpoint(TRUNCATE)") != 0) throw StorageFailure(StorageProblem::busy);
}
fs::path safe_blob_path(const fs::path& root, std::string_view id) {
    if (id.size() != 64 || id.find_first_not_of("0123456789abcdef") != id.npos)
        throw std::runtime_error("Invalid blob identity");
    const auto blobs = root / "blobs", shard = blobs / std::string(id.substr(0,2));
    const auto file = shard / (std::string(id) + ".blob");
    for (const auto& component : {root, blobs, shard, file}) refuse_reparse(component);
    return file;
}

Statement::Statement(sqlite3* db, const char* sql) { check(sqlite3_prepare_v2(db, sql, -1, &statement_, nullptr)); }
Statement::~Statement() { sqlite3_finalize(statement_); }
void Statement::bind(int i, std::string_view v) {
    check(sqlite3_bind_text64(statement_, i, v.empty() ? "" : v.data(), v.size(), SQLITE_TRANSIENT, SQLITE_UTF8));
}
void Statement::bind(int i, std::wstring_view value) {
    check(sqlite3_bind_text16(statement_,i,value.empty() ? L"" : value.data(),int(value.size()*2),SQLITE_TRANSIENT));
}
std::wstring Statement::wide(int i) const {
    auto data = static_cast<const wchar_t*>(sqlite3_column_text16(statement_,i));
    return data ? std::wstring(data,sqlite3_column_bytes16(statement_,i)/2) : std::wstring{};
}
void Statement::bind(int i, const char* v) { if (v) bind(i, std::string_view(v)); else bind(i, nullptr); }
void Statement::bind(int i, int v) { check(sqlite3_bind_int(statement_, i, v)); }
void Statement::bind(int i, std::int64_t v) { check(sqlite3_bind_int64(statement_, i, v)); }
void Statement::bind(int i, double v) { check(sqlite3_bind_double(statement_, i, v)); }
void Statement::bind(int i, std::nullptr_t) { check(sqlite3_bind_null(statement_, i)); }
void Statement::bind_blob(int i, std::span<const unsigned char> v) {
    if (v.empty()) check(sqlite3_bind_zeroblob(statement_, i, 0));
    else check(sqlite3_bind_blob64(statement_, i, v.data(), v.size(), SQLITE_TRANSIENT));
}
bool Statement::step() {
    const auto rc = sqlite3_step(statement_);
    if (rc == SQLITE_DONE) return false;
    if (rc != SQLITE_ROW) check(rc);
    return true;
}
void Statement::run() { if (step()) throw std::logic_error("Unexpected history query result"); }
void Statement::reset() { check(sqlite3_reset(statement_)); }
std::string Statement::text(int i) const {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(statement_, i));
    return p ? std::string(p, sqlite3_column_bytes(statement_, i)) : std::string{};
}
std::int64_t Statement::integer(int i) const { return sqlite3_column_int64(statement_, i); }
double Statement::real(int i) const { return sqlite3_column_double(statement_, i); }
bool Statement::is_null(int i) const { return sqlite3_column_type(statement_, i) == SQLITE_NULL; }
std::vector<unsigned char> Statement::blob(int i) const {
    const auto* p = static_cast<const unsigned char*>(sqlite3_column_blob(statement_, i));
    return p ? std::vector<unsigned char>(p, p + sqlite3_column_bytes(statement_, i)) : std::vector<unsigned char>{};
}

Transaction::Transaction(sqlite3* db, std::unique_lock<std::recursive_mutex>* yield_while_busy) : db_(db) {
    if (!yield_while_busy) { exec(db_, "BEGIN IMMEDIATE"); return; }
    // A background edit must not hold the shared connection mutex while an
    // external WAL writer is busy. Restore the connection policy before yielding.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(writer_timeout_ms);
    for (;;) {
        sqlite3_busy_timeout(db_, 0);
        const int rc = sqlite3_exec(db_, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
        sqlite3_busy_timeout(db_, writer_timeout_ms);
        if (rc != SQLITE_BUSY || std::chrono::steady_clock::now() >= deadline) { check(rc); return; }
        yield_while_busy->unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        yield_while_busy->lock();
    }
}
Transaction::~Transaction() { if (db_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
void Transaction::commit() { exec(db_, "COMMIT"); db_ = nullptr; }

Database::Database(const fs::path& root, Fault inject)
    : root_(fs::absolute(root).lexically_normal()), inject_(std::move(inject)) {
    if (root.empty()) throw std::runtime_error("A history directory is required");
    fs::create_directories(root_);
    refuse_reparse(root_);
    const auto lock_path = root_ / ".writer.lock";
    refuse_reparse(lock_path);
    // Standalone backups have no writer lock. Refuse an unsupported source
    // before creating the first file in its directory.
    if (!fs::exists(lock_path) && fs::exists(root_ / "history.db"))
        preflight_history(root_);
    process_lock_ = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                OPEN_ALWAYS, FILE_ATTRIBUTE_HIDDEN, nullptr);
    if (process_lock_ == INVALID_HANDLE_VALUE)
        throw StorageFailure(GetLastError()==ERROR_SHARING_VIOLATION?StorageProblem::busy:StorageProblem::access);
    try {
        const auto file = root_ / "history.db";
        for (const auto& name : {"history.db","history.db-wal","history.db-shm","history.db-journal"})
            refuse_reparse(root_ / name);
        const bool create = !fs::exists(file);
        if ((!create && fs::file_size(file) == 0) ||
            (create && (fs::exists(root_/"blobs") || fs::exists(root_/"history.db-wal") ||
                        fs::exists(root_/"history.db-shm") || fs::exists(root_/"history.db-journal"))))
            throw StorageFailure(StorageProblem::damaged_database);
        if (create) create_history(root_, inject_);
        else preflight_history(root_);
        db_ = open_sql(file, SQLITE_OPEN_READWRITE);
        check(sqlite3_busy_timeout(db_, writer_timeout_ms));
        require_current_history(db_);
        exec(db_, "PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL");
        verify_metadata(db_);
    } catch (...) { sqlite3_close(db_); CloseHandle(process_lock_); throw; }
}
// The store keeps the owner's profile lock until its capture writer drains. A separate
// WAL connection lets the STA read the last commit throughout blob preparation and SQL.
Database::Database(const Database& owner, AdmissionConnection)
    : root_(owner.root_), inject_(owner.inject_) {
    db_ = open_sql(root_ / "history.db", SQLITE_OPEN_READWRITE);
    try {
        check(sqlite3_busy_timeout(db_, writer_timeout_ms));
        require_current_history(db_);
        exec(db_, "PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL");
    } catch (...) { sqlite3_close(db_); throw; }
}
Database::~Database() { sqlite3_close(db_); if (process_lock_) CloseHandle(process_lock_); }
Database::Database(const Database& owner, SnapshotConnection)
    : root_(owner.root_), inject_(owner.inject_) {
    db_ = open_sql(root_ / "history.db", SQLITE_OPEN_READONLY);
    try { require_current_history(db_); exec(db_, "PRAGMA query_only=ON"); }
    catch (...) { sqlite3_close(db_); throw; }
}
void Database::fault(std::string_view point) const { if (inject_) inject_(point); }

namespace {
using Connection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
struct CloseFile {
    void operator()(void *handle) const {
        CloseHandle(handle);
    }
};
struct ExternalBlob {
    fs::path source, relative;
    std::string id;
    std::int64_t size;
    std::unique_ptr<void, CloseFile> lease;
};
struct Snapshot {
    Connection read{nullptr, sqlite3_close};
    std::vector<ExternalBlob> blobs;
};
// Both exported payload kinds use the same ownership rule. A deleted entry can
// leave all its content rows present until the next live collection.
constexpr char reachable_blobs[] =
    "SELECT b.id,b.size,b.relative_path,b.inline_bytes FROM blobs b "
    "WHERE (b.relative_path IS NOT NULL)=:external AND EXISTS("
    "SELECT 1 FROM representations r JOIN entries e ON e.snapshot_id=r.snapshot_id "
    "WHERE r.blob_id=b.id) ORDER BY b.id";
std::unique_ptr<void, CloseFile> offline_guard(const fs::path &root) {
    const auto path = root / "history.db";
    refuse_reparse(root);
    refuse_reparse(path);
    // Hold the offline source against writers throughout verification or export.
    auto handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        switch (GetLastError()) {
        case ERROR_SHARING_VIOLATION:
            throw StorageFailure(StorageProblem::busy);
        case ERROR_FILE_NOT_FOUND:
        case ERROR_PATH_NOT_FOUND:
            throw StorageFailure(StorageProblem::damaged_database);
        default:
            throw StorageFailure(StorageProblem::access);
        }
    }
    std::unique_ptr<void, CloseFile> guard(handle);
    preflight_history(root);
    return guard;
}
Snapshot freeze(const fs::path &root) {
    refuse_reparse(root);
    const auto file = root / "history.db";
    for (const auto *name : {"history.db", "history.db-wal", "history.db-shm", "history.db-journal"})
        refuse_reparse(root / name);
    if (!fs::is_regular_file(file))
        throw StorageFailure(StorageProblem::damaged_database);
    Snapshot snapshot;
    snapshot.read.reset(open_sql(file, SQLITE_OPEN_READONLY));
    auto *read = snapshot.read.get();
    exec(read, "PRAGMA query_only=ON; BEGIN");
    // The first read fixes this WAL snapshot. The writer lock excludes GC
    // until the final immutable file has its no-share-delete lease.
    require_current_history(read);
    Statement list(read, reachable_blobs);
    list.bind(1, 1);
    while (list.step()) {
        auto id = list.text(0);
        auto source = safe_blob_path(root, id);
        auto local = source.lexically_relative(root);
        auto stored = list.text(2);
        std::replace(stored.begin(), stored.end(), '\\', '/');
        if (local.generic_string() != stored)
            throw StorageFailure(StorageProblem::damaged_database);
        HANDLE h = CreateFileW(source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            const auto code = GetLastError();
            if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND)
                throw StorageFailure(StorageProblem::missing_blob);
            throw StorageFailure(StorageProblem::access);
        }
        ExternalBlob blob{std::move(source), std::move(local), std::move(id), list.integer(1),
                          std::unique_ptr<void, CloseFile>(h)};
        snapshot.blobs.push_back(std::move(blob));
    }
    return snapshot;
}
void verify_reachable_manifests(sqlite3* db) {
    Statement snapshots(db, "SELECT id,substr(manifest_hash,1,65) FROM snapshots s "
                            "WHERE EXISTS(SELECT 1 FROM entries e WHERE e.snapshot_id=s.id)");
    while (snapshots.step()) {
        Statement rows(db, "SELECT standard_id,substr(format_name,1,1025),tymed,aspect,lindex,codec_version,priority,"
                           "status,byte_count,blob_id FROM representations WHERE snapshot_id=? ORDER BY ordinal LIMIT 257");
        rows.bind(1, snapshots.text(0));
        std::vector<unsigned char> manifest;
        size_t count = 0, total = 0;
        bool usable = false;
        while (rows.step()) {
            if (++count > max_formats) throw StorageFailure(StorageProblem::damaged_manifest);
            const auto format = read_manifest_format(rows);
            append_manifest(manifest, format, rows.text(9));
            if (format.outcome == "saved") {
                const auto length = rows.integer(8);
                if (length < 0 || static_cast<std::uint64_t>(length) > std::min(max_blob, max_snapshot - total))
                    throw StorageFailure(StorageProblem::damaged_manifest);
                total += static_cast<size_t>(length);
                usable = true;
            }
        }
        if (!usable) throw StorageFailure(StorageProblem::damaged_manifest);
        verify_manifest(manifest, snapshots.text(1));
    }
}
void verify_snapshot_sql(sqlite3 *db) {
    verify_metadata(db);
    Statement integrity(db, "PRAGMA integrity_check(1)");
    if (!integrity.step() || integrity.text(0) != "ok")
        throw StorageFailure(StorageProblem::damaged_database);
    verify_foreign_keys(db);
    if (scalar(db, "SELECT count(*) FROM representations r LEFT JOIN blobs b ON r.blob_id=b.id "
                   "WHERE r.status='saved' AND (b.id IS NULL OR r.byte_count<>b.size) "
                   "AND EXISTS(SELECT 1 FROM entries e WHERE e.snapshot_id=r.snapshot_id)"))
        throw StorageFailure(StorageProblem::damaged_database);
    Statement inline_blobs(db, reachable_blobs);
    inline_blobs.bind(1, 0);
    while (inline_blobs.step()) {
        auto bytes = inline_blobs.blob(3);
        if (static_cast<std::int64_t>(bytes.size()) != inline_blobs.integer(1) ||
            sha256(bytes) != inline_blobs.text(0))
            throw StorageFailure(StorageProblem::damaged_blob);
    }
    verify_reachable_manifests(db);
}
void export_snapshot(Snapshot snapshot, const fs::path &root, const fs::path &destination, bool allow_empty,
                     const Database::Fault &inject) {
    if (destination.empty())
        throw std::runtime_error("A new backup or restore folder is required");
    const auto target = fs::absolute(destination).lexically_normal();
    const bool existing = fs::exists(target);
    if (existing && (!allow_empty || !fs::is_directory(target) || !fs::is_empty(target)))
        throw std::runtime_error("Destination must be a new or empty folder; existing history was preserved");
    refuse_reparse(target);
    if (target == target.root_path())
        throw std::runtime_error("Choose a dedicated backup or restore folder");
    const auto relative = target.lexically_relative(root);
    if (!relative.empty() && *relative.begin() != ".." && (allow_empty || *relative.begin() != "backups"))
        throw std::runtime_error("Choose a separate restore folder or a store-local backups folder");
    fs::create_directories(target.parent_path());
    const auto staging = target.parent_path() / ("cliprope-partial-" + unique_suffix());
    if (!fs::create_directory(staging))
        throw std::runtime_error("Could not create a new snapshot staging directory");
    bool removed_empty = false;
    try {
        {
            Connection copy(open_sql(staging / "history.db"), sqlite3_close);
            auto *backup = sqlite3_backup_init(copy.get(), "main", snapshot.read.get(), "main");
            if (!backup)
                check(sqlite3_errcode(copy.get()));
            const auto rc = sqlite3_backup_step(backup, -1);
            const auto end = sqlite3_backup_finish(backup);
            if (rc != SQLITE_DONE)
                check(rc);
            check(end);
            snapshot.read.reset(); // WAL checkpoints can proceed while the export is compacted.
            // Work only on the new copy. Cascading snapshot deletion removes
            // its representations. VACUUM also omits previously freed pages
            // and deleted inline bytes, even when the source already ran GC.
            exec(copy.get(), "DELETE FROM managed_screenshots"); // Generated PNGs stay with their owning profile.
            exec(copy.get(), "PRAGMA journal_mode=DELETE; PRAGMA foreign_keys=ON; PRAGMA secure_delete=ON;"
                             "BEGIN;"
                             "DELETE FROM snapshots WHERE NOT EXISTS("
                             "SELECT 1 FROM entries e WHERE e.snapshot_id=snapshots.id);"
                             "DELETE FROM blobs WHERE NOT EXISTS("
                             "SELECT 1 FROM representations r WHERE r.blob_id=blobs.id);"
                             "COMMIT; VACUUM");
            verify_snapshot_sql(copy.get());
        }
        for (const auto &blob : snapshot.blobs) {
            const auto output = staging / blob.relative;
            fs::create_directories(output.parent_path());
            std::error_code ec;
            if (!fs::copy_file(blob.source, output, ec))
                throw StorageFailure(ec.value() == ERROR_DISK_FULL ? StorageProblem::full
                                                                   : StorageProblem::access);
            check_blob_copy(output, blob.size, blob.id);
        }
        if (inject)
            inject("backup-install");
        if (existing) {
            // Only RemoveDirectory can remove an empty directory. Win32
            // refuses if anything appeared there after the preflight.
            if (!RemoveDirectoryW(target.c_str()))
                throw std::runtime_error("Restore destination is no longer empty");
            removed_empty = true;
        }
        install_backup(staging, target);
    } catch (...) {
        if (removed_empty)
            CreateDirectoryW(target.c_str(), nullptr);
        std::error_code ignored;
        fs::remove_all(staging, ignored);
        throw;
    }
}
} // namespace

void Database::backup_to(const fs::path &destination) {
    Snapshot snapshot;
    {
        std::lock_guard lock(mutex_);
        if (!sqlite3_get_autocommit(db_))
            throw std::logic_error("Cannot back up an uncommitted history operation");
        snapshot = freeze(root_);
    }
    fault("backup-pinned");
    export_snapshot(std::move(snapshot), root_, destination, false, inject_);
}
void Database::export_profile(const fs::path &source, const fs::path &destination) {
    const auto root = fs::absolute(source).lexically_normal();
    auto guard = offline_guard(root);
    export_snapshot(freeze(root), root, destination, false, {});
}
void Database::restore_backup(const fs::path &backup, const fs::path &destination) {
    const auto root = fs::absolute(backup).lexically_normal();
    auto guard = offline_guard(root);
    export_snapshot(freeze(root), root, destination, true, {});
}
void Database::verify_store(const fs::path &source) {
    const auto root = fs::absolute(source).lexically_normal();
    auto guard = offline_guard(root);
    auto snapshot = freeze(root);
    verify_snapshot_sql(snapshot.read.get());
    for (const auto &blob : snapshot.blobs)
        check_blob_copy(blob.source, blob.size, blob.id);
}
} // namespace cliprope::persistence
