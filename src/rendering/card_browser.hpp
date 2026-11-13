#pragma once
#include "persistence/browse.hpp"
#include "rendering/thumbnail_cache.hpp"

namespace cliprope::rendering {
// Desktop card projections share the read model with the headless store.
class CardBrowser final {
public:
    explicit CardBrowser(persistence::Browser records) : records_(records), thumbnails_(records.root()) {}
    domain::ImagePreferences image_preferences;
    application::CardSummary summary(std::string_view id, bool previews = false, unsigned width = preview_width);
    std::vector<application::CardSummary> page(std::string_view rope, size_t offset, size_t count,
                                              bool previews = false, unsigned width = preview_width,
                                              persistence::PageReadStats* stats = nullptr);
    enum class PathLookup { cached_preview, inspect_for_menu };
    application::CardSummary menu_summary(std::string_view id);
    void inspect_path_kind(application::CardSummary&, PathLookup);
    std::wstring information(const application::CardSummary&);
    std::wstring manifest(std::string_view entry_id);
    void on_preview_ready(std::function<void()> callback) { thumbnails_.on_ready(std::move(callback)); }
    void recheck_files(std::string requested_list = {}) { thumbnails_.recheck_files(std::move(requested_list)); }
    application::PreviewChanges take_preview_changes() { return thumbnails_.take_changes(); }
    std::size_t thumbnail_bytes() const { return thumbnails_.bytes(); }
private:
    persistence::Browser records_;
    ThumbnailCache thumbnails_;
    application::CardSummary describe(const persistence::StoredCard&, bool, unsigned, persistence::PageReadStats*);
};
} // namespace cliprope::rendering
