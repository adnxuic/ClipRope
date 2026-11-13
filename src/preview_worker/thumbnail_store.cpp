#include "preview_worker/thumbnail_store.hpp"
#include "cache/thumbnail_name.hpp"
#include "platform_win/file_group_paths.hpp"
#include <fstream>

namespace cliprope::preview_worker {
namespace fs = std::filesystem;
namespace {
using domain::ImagePreview;
bool regular(const fs::path& p) {
    const auto attrs = GetFileAttributesW(p.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}
std::uint32_t checksum(const std::vector<unsigned char>& bytes) {
    std::uint32_t hash = 2166136261u;
    for (const auto b : bytes) {
        hash ^= b;
        hash *= 16777619u;
    }
    return hash;
}
struct Header {
    std::uint32_t magic, width, height, sum;
};
constexpr std::uint32_t magic = 0x334d4854; // THM3 includes oriented source dimensions.

constexpr std::uint32_t file_magic = 0x32465243;
ImagePreview read(const fs::path& file, const FileVersion* version = nullptr, bool stale = false) {
    if (!regular(file.parent_path()) || !regular(file))
        return {};
    std::ifstream f(file, std::ios::binary);
    Header h{};
    if (!f.read(reinterpret_cast<char*>(&h), sizeof h) || !h.width || !h.height ||
        h.width > cache::thumbnail_widths.back() || h.height > cache::thumbnail_widths.back()) return {};
    if (h.magic != (version ? file_magic : magic)) return {};
    std::array<std::uint32_t, 3> source{};
    if (!f.read(reinterpret_cast<char*>(source.data()), sizeof source)) return {};
    if (source[0] > 32768 || source[1] > 32768 || source[2] > 31) return {};
    std::uint64_t revision{};
    if (version) {
        FileVersion saved{};
        if (!f.read(reinterpret_cast<char*>(saved.data()), sizeof saved) || (!stale && *version != saved))
            return {};
        revision = saved[1] ^ saved[0];
    }
    ImagePreview image{h.width, h.height, std::vector<unsigned char>(h.width * h.height * 4)};
    if (!f.read(reinterpret_cast<char*>(image.pbgra.data()), image.pbgra.size()) ||
        checksum(image.pbgra) != h.sum)
        return {};
    image.source = {source[0], source[1], source[2], revision};
    return image;
}
void write(const fs::path& file, const ImagePreview& image, const FileVersion* version = nullptr) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    if (ec || !regular(file.parent_path()))
        return;
    auto tmp = file;
    tmp += cache::staging_extension;
    // CREATE_NEW never follows an existing staging file or reparse point.
    HANDLE out =
        CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE)
        return;
    Header h{version ? file_magic : magic, image.width, image.height, checksum(image.pbgra)};
    DWORD count = 0;
    const std::array source{image.source.width, image.source.height, image.source.orientation};
    bool ok = WriteFile(out, &h, sizeof h, &count, nullptr) && count == sizeof h &&
              WriteFile(out, source.data(), sizeof source, &count, nullptr) && count == sizeof source &&
              (!version || (WriteFile(out, version->data(), sizeof *version, &count, nullptr) && count == sizeof *version)) &&
              WriteFile(out, image.pbgra.data(), static_cast<DWORD>(image.pbgra.size()), &count, nullptr) &&
              count == image.pbgra.size();
    CloseHandle(out);
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING))
        DeleteFileW(tmp.c_str());
}
} // namespace
ThumbnailStore::ThumbnailStore(const fs::path& profile, const ThumbnailRequest& r)
    : profile_(profile), directory_(profile / L"thumbnails"), hash_(r.hash.data(), r.hash.size()) {
    const auto database = profile / "history.db";
    if (!profile.is_absolute() || !regular(profile) || !regular(database))
        throw std::runtime_error("Thumbnail profile unavailable");
    persistence::check_closed_history(profile);
    const auto name = database.u8string();
    sqlite3* connection{};
    const auto rc = sqlite3_open_v2(reinterpret_cast<const char*>(name.c_str()), &connection,
                                    SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOFOLLOW, nullptr);
    db_.reset(connection);
    if (rc != SQLITE_OK) throw std::runtime_error("Could not open thumbnail source");
    sqlite3_busy_timeout(connection, 150);
    persistence::exec(connection, "PRAGMA trusted_schema=OFF");
    persistence::require_current_history(connection);
    const auto read_only = [](void*, int action, const char*, const char*, const char*, const char*) {
        return action == SQLITE_SELECT || action == SQLITE_READ || action == SQLITE_TRANSACTION ? SQLITE_OK : SQLITE_DENY;
    };
    if (sqlite3_set_authorizer(connection, read_only, nullptr) != SQLITE_OK)
        throw std::runtime_error("Cannot restrict thumbnail connection");
    std::string_view adapter = cache::files;
    switch (r.op) {
    case ThumbnailOp::png: adapter = cache::png; break;
    case ThumbnailOp::dib: adapter = cache::dib; format_ = CF_DIB; break;
    case ThumbnailOp::dibv5: adapter = cache::dibv5; format_ = CF_DIBV5; break;
    default: format_ = CF_HDROP; break;
    }
    file_ = directory_ / fs::path(cache::thumbnail_key(hash_, adapter, r.width)).concat(cache::extension);
}

bool ThumbnailStore::reachable() {
    persistence::Statement live(db_.get(), "SELECT 1 FROM representations r JOIN entries e ON e.snapshot_id=r.snapshot_id "
        "WHERE r.blob_id=?1 AND r.status='saved' AND ((?2>0 AND r.standard_id=?2) OR (?2=0 AND r.format_name='PNG' COLLATE NOCASE)) LIMIT 1");
    live.bind(1, hash_);
    live.bind(2, static_cast<std::int64_t>(format_));
    return live.step();
}

std::vector<unsigned char> ThumbnailStore::load() {
    persistence::Statement row(db_.get(), "SELECT size,relative_path,inline_bytes FROM blobs WHERE id=?");
    row.bind(1, hash_);
    const auto limit = format_ == CF_HDROP ? max_group_bytes : 32u * 1024 * 1024;
    if (!row.step() || row.integer(0) <= 0 || static_cast<uint64_t>(row.integer(0)) > limit)
        throw std::runtime_error("Thumbnail input out of range");
    std::vector<unsigned char> bytes;
    if (row.is_null(1)) bytes = row.blob(2);
    else {
        const auto path = persistence::safe_blob_path(profile_, hash_);
        std::ifstream input(path, std::ios::binary);
        bytes.resize(static_cast<size_t>(row.integer(0)));
        if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
            throw std::runtime_error("Incomplete thumbnail source");
    }
    if (bytes.size() != static_cast<size_t>(row.integer(0)) || persistence::sha256(bytes) != hash_)
        throw std::runtime_error("Damaged thumbnail source");
    return bytes;
}

domain::ImagePreview ThumbnailStore::cached(const FileVersion* version, bool stale) {
    auto image = read(file_, version, stale);
    if (version && stale) {
        for (auto it = cache::thumbnail_widths.rbegin(); it != cache::thumbnail_widths.rend() && image.pbgra.empty(); ++it) {
            const auto alternate = cache::thumbnail_key(hash_, cache::files, *it);
            image = read(directory_ / fs::path(alternate).concat(cache::extension), version, true);
        }
    }
    return image;
}

void ThumbnailStore::install(const domain::ImagePreview& image, const FileVersion* version) {
    // Exclude a concurrent entry removal or collector between the live check and the atomic rename.
    // A derivative can be regenerated. A busy writer must not suppress the decoded pixels.
    try {
        persistence::Transaction guard(db_.get());
        if (reachable()) write(file_, image, version);
        guard.commit();
    } catch (const std::exception&) {
        // The caller rechecks reachability before returning any pixels.
    }
}
} // namespace cliprope::preview_worker
