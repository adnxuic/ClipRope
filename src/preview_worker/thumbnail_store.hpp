#pragma once
#include "preview_worker/thumbnail.hpp"
#include "persistence/database.hpp"
#include <memory>

namespace cliprope::preview_worker {
// One scoped connection in the ordinary helper. It never opens a HistoryStore or migrates a profile.
class ThumbnailStore {
public:
    ThumbnailStore(const std::filesystem::path&, const ThumbnailRequest&);
    bool reachable();
    std::vector<unsigned char> load();
    domain::ImagePreview cached(const FileVersion*, bool stale = false);
    void install(const domain::ImagePreview&, const FileVersion*);
private:
    std::filesystem::path profile_, directory_, file_;
    std::string hash_;
    unsigned format_{};
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db_{nullptr, sqlite3_close};
};
} // namespace cliprope::preview_worker
