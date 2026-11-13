#include "cache/thumbnail_name.hpp"
#include "persistence/content.hpp"
#include "persistence/file_digest.hpp"
#include "persistence/manifest.hpp"
#include "persistence/search_fields.hpp"
#include "platform_win/search_text.hpp"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <thread>

namespace cliprope::persistence {
namespace fs = std::filesystem;
namespace {
using Bytes = std::vector<unsigned char>;

std::string identifier(sqlite3* db) {
    Statement id(db, "SELECT lower(hex(randomblob(16)))");
    if (!id.step()) throw std::runtime_error("Could not allocate a content identity");
    return id.text(0);
}

void check_directory(const fs::path& path) {
    fs::create_directories(path);
    const auto flags = GetFileAttributesW(path.c_str());
    if (flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !(flags & FILE_ATTRIBUTE_DIRECTORY)) throw std::runtime_error("Invalid content directory");
}

// Locks out deletion, including by another process after this store closes.
// Leased payload bytes also remain usable when a view releases its entry.
struct File {
    explicit File(HANDLE value) : handle(value) {}
    ~File() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    HANDLE handle;
};

bool matches_length(const File& file, std::size_t expected) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.handle, &size)) throw StorageFailure(StorageProblem::damaged_blob);
    if (size.QuadPart != static_cast<LONGLONG>(expected)) return false;
    if (expected > max_blob) throw StorageFailure(StorageProblem::damaged_blob);
    return true;
}

Bytes read_file(const File& file, std::size_t expected, std::stop_token stop = {}) {
    if (!matches_length(file, expected)) throw StorageFailure(StorageProblem::damaged_blob);
    Bytes bytes(expected);
    for (std::size_t offset = 0; offset < expected;) {
        if (stop.stop_requested()) throw std::runtime_error("Snapshot read cancelled");
        const auto chunk = static_cast<DWORD>(std::min<std::size_t>(expected - offset, 1024 * 1024));
        DWORD read{};
        if (!ReadFile(file.handle, bytes.data() + offset, chunk, &read, nullptr) || read != chunk)
            throw std::runtime_error("Damaged content: external blob could not be read");
        offset += read;
    }
    return bytes;
}

std::shared_ptr<File> open_blob(const fs::path& path) {
    const auto handle=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
                                  FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
    if(handle==INVALID_HANDLE_VALUE){
        const auto code=GetLastError();
        if(code==ERROR_FILE_NOT_FOUND || code==ERROR_PATH_NOT_FOUND)throw StorageFailure(StorageProblem::missing_blob);
        throw StorageFailure(StorageProblem::access);
    }
    return std::make_shared<File>(handle);
}

void write_part(HANDLE file, std::span<const unsigned char> bytes) {
    DWORD written{};
    if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
        written != bytes.size()) throw std::runtime_error("Could not write staged content");
}

void install_file(Database& db, const std::string& hash, std::span<const unsigned char> bytes) {
    const auto destination = safe_blob_path(db.root(), hash);
    check_directory(destination.parent_path());
    if (fs::exists(destination)) {
        const auto existing = open_blob(destination);
        if (!matches_length(*existing, bytes.size())) throw StorageFailure(StorageProblem::damaged_blob);
        if (file_sha256(existing->handle) != hash)
            throw std::runtime_error("Damaged content: existing blob failed verification");
        return;
    }
    const auto staging = db.root() / "staging";
    check_directory(staging);
    const auto stage = staging / (identifier(db.handle()) + ".part");
    try {
        {
            File file(CreateFileW(stage.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr));
            if (file.handle == INVALID_HANDLE_VALUE) {
                const auto error=GetLastError();
                if(error==ERROR_ACCESS_DENIED || error==ERROR_SHARING_VIOLATION)throw StorageFailure(StorageProblem::access);
                if(error==ERROR_DISK_FULL)throw StorageFailure(StorageProblem::full);
                throw std::runtime_error("Could not create a staged file. Check free space and profile permissions.");
            }
            auto first = bytes.first(bytes.size() / 2);
            write_part(file.handle, first);
            db.fault("staging");
            write_part(file.handle, bytes.subspan(first.size()));
            if (!FlushFileBuffers(file.handle)) throw std::runtime_error("Could not flush staged content");
        }
        if (!MoveFileExW(stage.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Could not install immutable content");
    } catch (...) {
        std::error_code ignored;
        fs::remove(stage, ignored);
        throw;
    }
    db.fault("installed");
}

void store_blob(Database& db, const std::string& hash, const Bytes& bytes) {
    const bool external = bytes.size() > ContentRepository::inline_limit;
    if (external) install_file(db, hash, bytes);
    Statement insert(db.handle(), "INSERT INTO blobs(id,size,inline_bytes,relative_path) VALUES(?,?,?,?) "
                                  "ON CONFLICT(id) DO NOTHING");
    insert.bind(1, hash);
    insert.bind(2, static_cast<std::int64_t>(bytes.size()));
    if (external) {
        insert.bind(3, nullptr);
        insert.bind(4, "blobs/" + hash.substr(0, 2) + "/" + hash + ".blob");
    } else {
        insert.bind_blob(3, bytes);
        insert.bind(4, nullptr);
    }
    insert.run();
    if (!sqlite3_changes(db.handle())) {
        Statement existing(db.handle(), "SELECT size,inline_bytes,relative_path FROM blobs WHERE id=?");
        existing.bind(1, hash);
        if (!existing.step() || existing.integer(0) != static_cast<std::int64_t>(bytes.size()) ||
            (external ? existing.text(2) != "blobs/" + hash.substr(0, 2) + "/" + hash + ".blob"
                      : existing.blob(1) != bytes))
            throw std::runtime_error("Damaged content: stored blob does not match its identity");
    }
}


// A lease can outlive its original connection. Index by absolute profile path.
using Key = std::pair<fs::path, std::string>;
std::mutex lease_mutex;
std::map<Key, std::weak_ptr<void>> leases;
std::shared_ptr<void> pin(const fs::path& root, const std::string& hash) {
    std::lock_guard lock(lease_mutex);
    auto& weak = leases[{root, hash}];
    auto token = weak.lock();
    if (!token) weak = token = std::make_shared<char>();
    return token;
}
std::set<std::string> pinned(const fs::path& root) {
    std::set<std::string> result;
    std::lock_guard lock(lease_mutex);
    std::erase_if(leases, [](const auto& item) { return item.second.expired(); });
    for (const auto& [key, token] : leases)
        if (key.first == root && !token.expired()) result.insert(key.second);
    return result;
}

bool hex_name(std::string_view name, size_t size) {
    return name.size() == size && name.find_first_not_of("0123456789abcdef") == name.npos;
}
bool reparse(const fs::path& path) {
    const auto flags = GetFileAttributesW(path.c_str());
    return flags == INVALID_FILE_ATTRIBUTES || (flags & FILE_ATTRIBUTE_REPARSE_POINT);
}

// Called only after grammar, reachability and link checks under the store lock.
bool reclaim_file_impl(const fs::path& path, CollectionResult& result) {
    if (DeleteFileW(path.c_str())) {
        ++result.removed;
        return true;
    }
    switch (GetLastError()) {
    case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND:
        return true;
    case ERROR_ACCESS_DENIED: case ERROR_LOCK_VIOLATION: case ERROR_SHARING_VIOLATION:
        ++result.deferred;
        return false;
    default:
        throw std::runtime_error("Could not reclaim an unused content file");
    }
}

bool reclaim_file([[maybe_unused]] Database& db, const fs::path& path, CollectionResult& result) {
#ifdef CLIPROPE_STORAGE_PROBES
    db.fault("gc-delete-start");
#endif
    const bool reclaimed = reclaim_file_impl(path, result);
#ifdef CLIPROPE_STORAGE_PROBES
    db.fault("gc-delete-done");
#endif
    return reclaimed;
}

void sweep_files(Database& db, const std::set<std::string>& live, CollectionResult& result) {
    const auto staging = db.root() / "staging";
    const auto blobs = db.root() / "blobs";
    check_directory(staging);
    check_directory(blobs);
    for (const auto& file : fs::directory_iterator(staging)) {
        const auto& path = file.path();
        if (!reparse(path) && file.is_regular_file() && path.extension() == ".part" &&
            hex_name(path.stem().string(), 32)) {
            std::lock_guard guard(db.mutex());
            if (!reparse(path)) reclaim_file(db, path, result);
        }
    }
    for (const auto& shard : fs::directory_iterator(blobs)) {
        if (reparse(shard.path()) || !shard.is_directory() ||
            !hex_name(shard.path().filename().string(), 2)) continue;
        for (const auto& file : fs::directory_iterator(shard.path())) {
            const auto& path = file.path();
            const auto hash = path.stem().string();
            if (reparse(path) || !file.is_regular_file() || path.extension() != ".blob" ||
                !hex_name(hash, 64) || hash.substr(0, 2) != shard.path().filename().string() ||
                live.contains(hash)) continue;
            std::lock_guard guard(db.mutex());
            Statement current(db.handle(), "SELECT 1 FROM blobs WHERE id=?");
            current.bind(1, hash);
            if (!current.step() && !pinned(db.root()).contains(hash) && !reparse(path))
                reclaim_file(db, path, result);
        }
    }
}
void sweep_thumbnails(Database& db, CollectionResult& result) {
    const auto dir = db.root() / "thumbnails";
    if (!fs::exists(dir) || reparse(dir) || !fs::is_directory(dir))
        return;
    std::set<std::string> reachable, file_lists;
    {
        std::lock_guard guard(db.mutex());
        Statement query(
            db.handle(),
            "SELECT DISTINCT r.blob_id, r.standard_id FROM representations r JOIN entries e ON e.snapshot_id=r.snapshot_id "
            "WHERE r.status='saved' AND (r.standard_id IN (8,15,17) OR r.format_name='PNG' COLLATE NOCASE)");
        while (query.step())
            (query.integer(1) == 15 ? file_lists : reachable).insert(query.text(0));
    }
    for (const auto& file : fs::directory_iterator(dir)) {
        const auto& path = file.path();
        if (reparse(path) || !file.is_regular_file())
            continue;
        const auto filename = path.filename().u8string();
        const auto name = cache::parse_thumbnail_name({reinterpret_cast<const char*>(filename.data()), filename.size()});
        if (!name) continue;
        if (!name->staged && (name->file_list ? file_lists : reachable).contains(std::string(name->hash)))
            continue;
        // A separately supervised ordinary thumbnail can install between enumeration and reclamation.
        std::unique_lock guard(db.mutex());
        Transaction claim(db.handle(), &guard);
        Statement live(db.handle(), "SELECT 1 FROM representations r JOIN entries e ON e.snapshot_id=r.snapshot_id "
            "WHERE r.blob_id=?1 AND r.status='saved' AND ((?2=1 AND r.standard_id=15) OR "
            "(?2=0 AND (r.standard_id IN (8,17) OR r.format_name='PNG' COLLATE NOCASE))) LIMIT 1");
        live.bind(1, name->hash);
        live.bind(2, static_cast<std::int64_t>(name->file_list));
        if (!name->staged && live.step()) continue;
        if (!reparse(path)) reclaim_file(db, path, result);
        claim.commit();
    }
}
} // namespace

PreparedContent::PreparedContent(const domain::Snapshot& value, const Database::Fault& probe) : snapshot(value) {
    if (snapshot.formats.empty() || snapshot.formats.size() > max_formats)
        throw std::invalid_argument("Invalid representation count");
    std::size_t total = 0;
    for (const auto& format : snapshot.formats) {
        if ((format.standard_id == 0) == format.name.empty() || format.standard_id >= 0xc000 ||
            format.name.size() > 1024 || format.name.find('\0') != std::string::npos ||
            format.bytes.size() > max_blob || format.bytes.size() > max_snapshot - total ||
            format.codec_version < 1 || (format.outcome != "saved" && !format.bytes.empty()))
            throw std::invalid_argument("Invalid representation manifest");
        total += format.bytes.size();
    }
    fields = win::extract_search_fields(snapshot, probe);
}
std::string ContentRepository::install(const domain::Snapshot& snapshot) {
    return install(PreparedContent(snapshot));
}
std::string ContentRepository::install(const PreparedContent& content) {
    if (sqlite3_get_autocommit(db_.handle()))
        throw std::logic_error("Install content inside the entry transaction");
    const auto& snapshot = content.snapshot;
    const auto id = identifier(db_.handle());
    Statement insert(db_.handle(), "INSERT INTO snapshots(id,source_hint,capture_status,captured_at,screenshot_path,path_kind,snip_directory,snip_begin,snip_end) VALUES(?,?,?,?,?,?,?,?,?)");
    insert.bind(1, id);
    insert.bind(2, snapshot.source_hint);
    insert.bind(3, snapshot.capture_status);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    insert.bind(4, snapshot.captured_at ? snapshot.captured_at : seconds);
    insert.bind(5, snapshot.screenshot_path);
    insert.bind(6, static_cast<int>(snapshot.path_kind));
    insert.bind(7, snapshot.snip_directory);
    insert.bind(8, snapshot.snip_begin);
    insert.bind(9, snapshot.snip_end);
    insert.run();
    Bytes manifest;
    int ordinal = 0;
    bool usable = false;
    for (const auto& format : snapshot.formats) {
        std::string hash;
        if (format.outcome == "saved") {
            hash = sha256(format.bytes);
            store_blob(db_, hash, format.bytes);
            usable = true;
        }
        append_manifest(manifest, format, hash);
        Statement row(db_.handle(), "INSERT INTO representations(snapshot_id,ordinal,standard_id,format_name,"
            "tymed,aspect,lindex,codec_version,priority,status,byte_count,blob_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
        row.bind(1, id);
        row.bind(2, ordinal++);
        if (format.standard_id) { row.bind(3, static_cast<int>(format.standard_id)); row.bind(4, nullptr); }
        else { row.bind(3, nullptr); row.bind(4, format.name); }
        row.bind(5, static_cast<std::int64_t>(format.tymed));
        row.bind(6, static_cast<std::int64_t>(format.aspect));
        row.bind(7, format.index);
        row.bind(8, format.codec_version);
        row.bind(9, format.priority);
        row.bind(10, format.outcome);
        row.bind(11, static_cast<std::int64_t>(format.bytes.size()));
        if (hash.empty()) row.bind(12, nullptr); else row.bind(12, hash);
        row.run();
    }
    if (!usable) throw std::invalid_argument("No recoverable representation");
    Statement seal(db_.handle(), "UPDATE snapshots SET manifest_hash=? WHERE id=?");
    seal.bind(1, sha256(manifest));
    seal.bind(2, id);
    seal.run();
    index_search_fields(db_.handle(), id, content.fields);
    return id;
}

application::ScreenshotSource ContentRepository::screenshot(const domain::Entry& expected,
    std::stop_token stop, application::PreviewDeadline deadline) {
    using namespace application;
    std::unique_lock lock(db_.mutex(), std::defer_lock);
    while (!lock.try_lock()) {
        if (preview_expired(stop, deadline)) throw std::runtime_error("Preview cancelled");
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (preview_expired(stop, deadline)) throw std::runtime_error("Preview cancelled");
    Statement live(db_.handle(), "SELECT substr(s.manifest_hash,1,65) FROM entries e JOIN snapshots s ON s.id=e.snapshot_id "
                                "WHERE e.id=? AND e.snapshot_id=? AND e.revision=?");
    live.bind(1, expected.id); live.bind(2, expected.snapshot_id); live.bind(3, expected.revision);
    if (!live.step()) throw std::runtime_error("Stale screenshot request");
    const auto manifest_hash = live.text(0);
    Statement rows(db_.handle(), "SELECT r.standard_id,substr(r.format_name,1,1025),r.tymed,r.aspect,r.lindex,r.codec_version,"
        "r.priority,substr(r.status,1,32),r.byte_count,substr(r.blob_id,1,65),b.size,"
        "CASE WHEN length(b.inline_bytes)<=4096 THEN b.inline_bytes END,substr(b.relative_path,1,96) "
        "FROM representations r LEFT JOIN blobs b ON r.blob_id=b.id WHERE r.snapshot_id=? ORDER BY r.ordinal LIMIT 257");
    rows.bind(1, expected.snapshot_id);
    ScreenshotSource result;
    Bytes manifest, inline_bytes;
    bool external = false;
    unsigned count = 0;
    while (rows.step()) {
        if (++count > max_formats) throw std::runtime_error("Invalid screenshot inventory");
        auto f = read_manifest_format(rows);
        const auto hash = rows.text(9);
        append_manifest(manifest, f, hash);
        if (f.outcome != "saved") continue;
        if (f.standard_id == CF_UNICODETEXT || f.standard_id == CF_HDROP ||
            !_stricmp(f.name.c_str(), "FileGroupDescriptorW") || !_stricmp(f.name.c_str(), "FileContents"))
            throw std::runtime_error("Not a screenshot card");
        const bool png = !f.standard_id && !_stricmp(f.name.c_str(), "PNG");
        if (!result.hash.empty() || (!png && f.standard_id != CF_DIB && f.standard_id != CF_DIBV5)) continue;
        const auto length = rows.integer(8);
        if (f.aspect != 1 || f.index != -1 || f.codec_version != 1 ||
            (f.tymed != 1 && !(png && f.tymed == 4)) || rows.is_null(10) || length <= 0 ||
            length > 32 * 1024 * 1024 || length != rows.integer(10))
            throw StorageFailure(StorageProblem::damaged_blob);
        result.encoding = png ? ScreenshotEncoding::png : f.standard_id == CF_DIB ? ScreenshotEncoding::dib : ScreenshotEncoding::dibv5;
        result.hash = hash; result.length = static_cast<size_t>(length);
        external = !rows.is_null(12);
        if (external) {
            // Validate the stored relative location without opening the payload while holding the writer lock.
            if (!hex_name(hash, 64) || rows.text(12) != "blobs/" + hash.substr(0,2) + "/" + hash + ".blob")
                throw StorageFailure(StorageProblem::damaged_blob);
        } else {
            if (result.length > inline_limit || rows.is_null(11)) throw StorageFailure(StorageProblem::damaged_blob);
            inline_bytes = rows.blob(11);
        }
    }
    if (result.hash.empty())
        throw StorageFailure(StorageProblem::damaged_blob);
    result.lease = pin(db_.root(), result.hash);
    const auto root = db_.root();
    lock.unlock();
    verify_manifest(manifest, manifest_hash);
    result.read = [root, hash = result.hash, length = result.length, external,
                   bytes = std::move(inline_bytes), lease = result.lease](std::stop_token token, PreviewDeadline until) {
        if (preview_expired(token, until)) throw std::runtime_error("Preview cancelled");
        Bytes data = bytes;
        if (external) {
            auto path = safe_blob_path(root, hash);
            File file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED, nullptr));
            if (file.handle == INVALID_HANDLE_VALUE) throw StorageFailure(StorageProblem::missing_blob);
            BY_HANDLE_FILE_INFORMATION info{};
            LARGE_INTEGER size{};
            if (!GetFileInformationByHandle(file.handle, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
                !GetFileSizeEx(file.handle, &size) || size.QuadPart != static_cast<LONGLONG>(length))
                throw StorageFailure(StorageProblem::damaged_blob);
            File event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
            if (!event.handle) throw std::runtime_error("Preview read event unavailable");
            data.resize(length);
            for (size_t offset = 0; offset < length;) {
                if (preview_expired(token, until)) throw std::runtime_error("Preview cancelled");
                OVERLAPPED io{}; io.hEvent = event.handle; io.Offset = static_cast<DWORD>(offset);
                ResetEvent(event.handle);
                const auto chunk = static_cast<DWORD>(std::min<size_t>(256 * 1024, length - offset));
                DWORD read{};
                if (!ReadFile(file.handle, data.data()+offset, chunk, &read, &io)) {
                    if (GetLastError() != ERROR_IO_PENDING) throw StorageFailure(StorageProblem::damaged_blob);
                    while (WaitForSingleObject(event.handle, 10) == WAIT_TIMEOUT) {
                        if (preview_expired(token, until)) {
                            CancelIoEx(file.handle, &io);
                            GetOverlappedResult(file.handle, &io, &read, TRUE); // Keep the buffer alive until cancellation completes.
                            throw std::runtime_error("Preview cancelled");
                        }
                    }
                    if (!GetOverlappedResult(file.handle, &io, &read, FALSE)) throw StorageFailure(StorageProblem::damaged_blob);
                }
                if (read != chunk) throw StorageFailure(StorageProblem::damaged_blob);
                offset += read;
            }
        }
        if (preview_expired(token, until)) throw std::runtime_error("Preview cancelled");
        if (data.size() != length || sha256(data) != hash) throw StorageFailure(StorageProblem::damaged_blob);
        return data;
    };
    return result;
}

domain::LeasedSnapshot ContentRepository::load(std::string_view id, std::stop_token stop) {
    std::lock_guard lock(db_.mutex());
#ifdef CLIPROPE_STORAGE_PROBES
    db_.fault("snapshot-read");
#endif
    domain::LeasedSnapshot result;
    auto hold = std::make_shared<std::vector<std::shared_ptr<void>>>();
    result.lease = hold;
    auto& snapshot = result.snapshot;
    Statement header(db_.handle(), "SELECT source_hint,capture_status,captured_at,manifest_hash,screenshot_path,path_kind,snip_directory,snip_begin,snip_end FROM snapshots WHERE id=?");
    header.bind(1, id);
    if (!header.step()) throw std::runtime_error("Snapshot unavailable");
    snapshot.source_hint = header.text(0);
    snapshot.capture_status = header.text(1);
    snapshot.captured_at = header.integer(2);
    snapshot.screenshot_path = header.text(4);
    snapshot.path_kind = static_cast<domain::PathKind>(header.integer(5));
    snapshot.snip_directory = header.text(6);
    snapshot.snip_begin = header.integer(7);
    snapshot.snip_end = header.integer(8);
    Statement rows(db_.handle(), "SELECT r.standard_id,substr(r.format_name,1,1025),r.tymed,r.aspect,r.lindex,r.codec_version,"
        "r.priority,r.status,r.byte_count,r.blob_id,b.size,b.inline_bytes,b.relative_path "
        "FROM representations r LEFT JOIN blobs b ON r.blob_id=b.id WHERE r.snapshot_id=? ORDER BY r.ordinal");
    rows.bind(1, id);
    Bytes manifest;
    std::size_t total = 0;
    while (rows.step()) {
        if (stop.stop_requested()) throw std::runtime_error("Snapshot read cancelled");
        if (snapshot.formats.size() >= max_formats) throw StorageFailure(StorageProblem::damaged_manifest);
        auto format = read_manifest_format(rows);
        const auto hash = rows.text(9);
        append_manifest(manifest, format, hash);
        if (format.outcome == "saved") {
            const auto length = rows.integer(8);
            if (rows.is_null(10) || length < 0 || length != rows.integer(10) ||
                static_cast<std::uint64_t>(length) > std::min(max_blob, max_snapshot - total))
                throw std::runtime_error("Damaged content: invalid blob size");
            total += static_cast<std::size_t>(length);
            if (rows.is_null(12)) {
                format.bytes = rows.blob(11);
            } else {
                auto path = safe_blob_path(db_.root(), hash);
                if (fs::path(rows.text(12)) != path.lexically_relative(db_.root()))
                    throw std::runtime_error("Damaged content: invalid blob location");
                auto file = open_blob(path);
                format.bytes = read_file(*file, static_cast<std::size_t>(length), stop);
                hold->push_back(std::move(file));
            }
            if (sha256(format.bytes) != hash) throw StorageFailure(StorageProblem::damaged_blob);
            hold->push_back(pin(db_.root(), hash));
        }
        snapshot.formats.push_back(std::move(format));
    }
    if (hold->empty()) throw std::runtime_error("Damaged content: no readable representation");
    verify_manifest(manifest, header.text(3));
    return result;
}

fs::path managed_screenshot(Database& db, const domain::Entry& entry, std::span<const unsigned char> bytes) {
    std::lock_guard lock(db.mutex());
    Statement kept(db.handle(), "SELECT 1 FROM entries WHERE id=? AND snapshot_id=?");
    kept.bind(1, entry.id);
    kept.bind(2, entry.snapshot_id);
    if (!kept.step() || !hex_name(entry.snapshot_id, 32)) throw std::runtime_error("Screenshot no longer available");
    const auto dir = db.root() / "screenshots";
    check_directory(dir);
    const auto file = dir / (entry.snapshot_id + ".png");
    Statement known(db.handle(), "SELECT 1 FROM managed_screenshots WHERE snapshot_id=?");
    known.bind(1, entry.snapshot_id);
    const bool owned = known.step();
    if (!owned && fs::exists(file)) throw std::runtime_error("Screenshot filename already in use");
    if (owned && fs::exists(file) && !reparse(file)) {
        const auto existing = open_blob(file);
        if (matches_length(*existing, bytes.size()) && file_sha256(existing->handle) == sha256(bytes)) return file;
    }
    const auto staging = db.root() / "staging";
    check_directory(staging);
    const auto temp = staging / (identifier(db.handle()) + ".part");
    {
        File writer(CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (writer.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot create screenshot");
        write_part(writer.handle, bytes);
    }
    if (!owned) {
        Statement reserve(db.handle(), "INSERT INTO managed_screenshots(snapshot_id) VALUES(?)");
        reserve.bind(1, entry.snapshot_id);
        reserve.run();
    }
    if (!MoveFileExW(temp.c_str(), file.c_str(), MOVEFILE_WRITE_THROUGH | (owned ? MOVEFILE_REPLACE_EXISTING : 0))) {
        DeleteFileW(temp.c_str());
        if (!owned) {
            Statement forget(db.handle(), "DELETE FROM managed_screenshots WHERE snapshot_id=?");
            forget.bind(1, entry.snapshot_id);
            forget.run();
        }
        throw std::runtime_error("Cannot install screenshot PNG");
    }
    return file;
}

CollectionResult ContentRepository::collect() {
    std::set<std::string> live;
    {
        std::lock_guard lock(db_.mutex());
        db_.fault("gc-sql-start");
        const auto protected_ids = pinned(db_.root());
        Transaction transaction(db_.handle());
        exec(db_.handle(), "DELETE FROM snapshots WHERE NOT EXISTS(SELECT 1 FROM entries WHERE snapshot_id=snapshots.id)");
        std::vector<std::string> garbage;
        Statement orphan(db_.handle(), "SELECT id FROM blobs WHERE NOT EXISTS(SELECT 1 FROM representations WHERE blob_id=blobs.id)");
        while (orphan.step()) if (!protected_ids.contains(orphan.text(0))) garbage.push_back(orphan.text(0));
        for (const auto& hash : garbage) {
            Statement remove(db_.handle(), "DELETE FROM blobs WHERE id=?");
            remove.bind(1, hash);
            remove.run();
        }
        transaction.commit();
        if (!garbage.empty()) db_.fault("gc");
        Statement retained(db_.handle(), "SELECT id FROM blobs WHERE relative_path IS NOT NULL");
        while (retained.step()) live.insert(retained.text(0));
    }
    CollectionResult result;
    db_.fault("gc-scan-start");
    sweep_files(db_, live, result);
    try {
        sweep_thumbnails(db_, result);
    } catch (const fs::filesystem_error&) {
        // Derivative files can be retried on the next sweep. Database errors
        // and failures in the authoritative blob store still reach the caller.
        ++result.deferred;
    }
    db_.fault("gc-scan-done");
    const auto dir = db_.root() / "screenshots";
    if (fs::exists(dir) && (reparse(dir) || !fs::is_directory(dir))) return result;
    std::vector<std::string> unused;
    {
        std::lock_guard guard(db_.mutex());
        Statement images(db_.handle(), "SELECT m.snapshot_id FROM managed_screenshots m "
                                       "WHERE NOT EXISTS(SELECT 1 FROM entries e WHERE e.snapshot_id=m.snapshot_id)");
        while (images.step()) unused.push_back(images.text(0));
    }
    for (const auto& id : unused) {
        if (!hex_name(id, 32)) continue;
        std::lock_guard guard(db_.mutex());
        Statement current(db_.handle(), "SELECT 1 FROM managed_screenshots WHERE snapshot_id=? AND "
                                        "NOT EXISTS(SELECT 1 FROM entries WHERE snapshot_id=managed_screenshots.snapshot_id)");
        current.bind(1, id);
        if (!current.step()) continue;
        const auto path = dir / (id + ".png");
        if (fs::exists(path) && (reparse(path) || !reclaim_file(db_, path, result))) continue;
        Statement forget(db_.handle(), "DELETE FROM managed_screenshots WHERE snapshot_id=?");
        forget.bind(1, id);
        forget.run();
    }
    return result;
}
} // namespace cliprope::persistence
