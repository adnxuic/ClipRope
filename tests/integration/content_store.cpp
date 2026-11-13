#include "persistence/history_store.hpp"
#include "history_fixture.hpp"

#include <windows.h>

#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
using namespace cliprope;
using namespace cliprope::persistence;

namespace {
using history_fixture::require;
int count(Database& db, const char* query) {
    Statement row(db.handle(), query);
    require(row.step(), "Missing aggregate row");
    return static_cast<int>(row.integer(0));
}
void run(const fs::path& root) {
    domain::Snapshot sample;
    domain::Format text;
    text.standard_id = CF_UNICODETEXT;
    text.bytes = {'A', 0, 0, 0};
    domain::Format large;
    large.name = "ClipRope synthetic bytes";
    large.bytes.resize(12000);
    for (std::size_t i = 0; i < large.bytes.size(); ++i)
        large.bytes[i] = static_cast<unsigned char>(i % 211);
    sample.formats = {text, large};
    const auto hash = sha256(large.bytes);
    const auto path = root / "blobs" / hash.substr(0, 2) / (hash + ".blob");
    bool checked_install = false;
    domain::LeasedSnapshot publication;
    {
        HistoryStore store(root, [&](std::string_view point) {
            if (point != "installed") return;
            require(fs::file_size(path) == large.bytes.size(), "Blob was incomplete at install");
            // The writer has not committed. A concurrent SQL reader sees old history.
            sqlite3* reader{};
            require(sqlite3_open_v2((root / "history.db").string().c_str(), &reader,
                                  SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "Could not open observer");
            {
                Statement rows(reader, "SELECT count(*) FROM representations");
                require(rows.step() && rows.integer(0) == 0, "Reference visible before admission commit");
            }
            sqlite3_close(reader);
            checked_install = true;
        });
        const auto rope = store.active_rope();
        const auto first = store.admit(rope, sample);
        require(checked_install, "Install boundary was not observed");
        auto other = store.admit(rope, sample);
        store.detach(other);
        require(!store.entry(other).rope_id, "Second entry did not detach");
        require(count(store.database(), "SELECT count(*) FROM blobs") == 2, "Blob deduplication failed");
        require(count(store.database(), "SELECT count(*) FROM blobs WHERE inline_bytes IS NOT NULL") == 1,
              "Small representation was not inline");
        store.erase(first);
        store.gc();
        publication = store.load(other);
        require(publication.snapshot.formats.at(0).bytes == text.bytes &&
              publication.snapshot.formats.at(1).bytes == large.bytes, "Floating-only content failed verification");
        store.erase(other);
        store.gc();
        require(fs::is_regular_file(path) && count(store.database(), "SELECT count(*) FROM blobs") == 2,
              "GC reclaimed a published blob");
    }
    HistoryStore reopened(root);
    require(fs::is_regular_file(path) && publication.snapshot.formats.at(1).bytes == large.bytes,
          "Publication lost bytes after reopening the writer");
    publication = {};
    reopened.gc();
    require(!fs::exists(path) && count(reopened.database(), "SELECT count(*) FROM blobs") == 0,
          "Unleased orphan survived collection");
    auto damaged = reopened.admit(reopened.active_rope(), sample);
    require(fs::remove(path), "Could not prepare missing-blob fixture");
    try {
        reopened.load(damaged);
        throw std::logic_error("Missing blob loaded as healthy");
    } catch (const std::runtime_error& error) {
        require(std::string_view(error.what()).starts_with("Damaged content"), "Missing blob was not diagnosed");
    }
    require(reopened.entry(damaged).id == damaged, "Diagnosis removed a damaged entry");
    domain::Snapshot invalid;
    invalid.formats.emplace_back().outcome = "failed";
    try {
        reopened.admit(reopened.active_rope(), invalid);
        throw std::logic_error("Invalid manifest admitted");
    } catch (const std::invalid_argument&) {}
    require(reopened.hanging(reopened.active_rope()).size() == 1, "Failed admission mutated history");
}
} // namespace

int main() {
    try {
        history_fixture::Directory directory;
        run(directory.path);
        std::cout << "Inline and external bytes, ordered staging, leases, GC and damage detection passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
