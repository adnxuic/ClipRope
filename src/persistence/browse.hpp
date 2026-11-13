#pragma once
#include "application/browse.hpp"
#include <filesystem>
#include <utility>

namespace cliprope::persistence {
class Database;
class HistoryStore;
enum class FileListExtent { prefix, full };
struct StoredCard {
    domain::Entry entry;
    application::CardStyle style;
    domain::CardScale scale;
    std::optional<std::wstring> text;
    bool text_unavailable{}, partial{}, damaged{}, hover_resolution{}, image_file_previews{};
    domain::PathKind path_kind{};
    std::vector<std::pair<unsigned, std::string>> formats;
    unsigned image_format{};
    std::string image_hash, file_hash;
    std::vector<unsigned char> image_header, file_bytes;
    size_t file_length{};
    bool descriptors{};
};
struct PageReadStats { std::size_t prefix_bytes{}, preview_requests{}; };
class Browser {
public:
    explicit Browser(HistoryStore& store);
    const std::filesystem::path& root() const;
    std::vector<domain::Id> floating_ids(bool visible, int limit = -1, std::string_view after = {});
    domain::Rope rope(std::string_view id);
    std::size_t rope_count(bool include_hidden = false);
    std::vector<domain::Rope> ropes(std::size_t offset, std::size_t limit);
    std::optional<std::size_t> rope_position(std::string_view id);

    StoredCard summary(std::string_view entry_id, FileListExtent extent = FileListExtent::prefix);
    std::vector<StoredCard> page(std::string_view rope_id, size_t offset, size_t count,
                                 FileListExtent extent = FileListExtent::prefix, PageReadStats* stats = nullptr);
    std::vector<unsigned char> file_list(std::string_view hash);
    domain::PathKind saved_path_kind(const domain::Entry&);
    std::size_t count(std::string_view rope_id);
    application::CardDetails card_details(const application::CardSummary&);
    application::CardDetails inventory_lines(std::string_view entry_id);
    std::optional<std::size_t> position(std::string_view rope_id, std::string_view entry_id);
    application::Anchor load_anchor(std::string_view rope_id);
private:
    Database& db_;
};
} // namespace cliprope::persistence
