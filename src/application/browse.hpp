#pragma once

#include "domain/history.hpp"
#include "domain/image_preview.hpp"
#include <utility>

namespace cliprope::application {
inline constexpr std::size_t card_text_prefix_units = 128;
struct CardStyle {
    double font_size=14;
    bool bold=false;
    domain::ColorSelection text_color, background_color;
    unsigned ink_rgb() const { return text_color.resolve(domain::ink_default).rgb(); }
    unsigned paper_rgb() const { return domain::composite(background_color.resolve(domain::paper_default), domain::paper_default); }
    double opacity=1;
    bool operator==(const CardStyle&) const = default;
};
// The visible template follows the retained content. It never changes the saved formats.
enum class CardKind { text, link, image, files, folder, rich, unavailable };
enum class CardStatus { complete, partial, damaged };
enum class CardMenuKind { text, screenshot, single_file, multiple_files, folder, other };
enum class CardFormat { text, html, rtf, image, file_references, virtual_files, count };
struct CardSummary {
    CardMenuKind menu = CardMenuKind::other;
    bool unclassified_path{};
    // A preview can observe a live single path before the host confirms it in history.
    domain::PathKind observed_path_kind = domain::PathKind::unknown;
    domain::Entry entry;
    std::wstring text;
    CardStyle style;
    // Presentation fills these labels from semantic read-model values.
    std::wstring capabilities;
    std::vector<CardFormat> formats;
    bool generated_text{}, empty_text{}, text_unavailable{};
    std::shared_ptr<const domain::ImagePreview> thumbnail;
    bool image_primary = false;
    bool hover_resolution = true;
    bool image_file_previews = true;
    CardKind kind = CardKind::text;
    CardStatus status = CardStatus::complete;
    // For file cards: the first item's folder and the number of saved items.
    // `more_items` marks a count read from a prefix of a longer list.
    std::wstring location;
    std::size_t items = 0;
    bool more_items = false;
    enum class FilePreviewKind { none, picture, collage };
    FilePreviewKind file_preview = FilePreviewKind::none;
    domain::FileAvailability availability = domain::FileAvailability::unchecked;
    std::string file_list_hash, pixel_blob_id;
    std::string_view preview_key() const { return file_list_hash.empty() ? pixel_blob_id : file_list_hash; }
    domain::ImageMetadata source;
    domain::ImagePreferences image_preferences;
    // When whitespace differs from the single-line menu label, preserve it on paper.
    std::optional<std::wstring> paper_text;

    domain::CardScale scale;

    void accept_text_edit(const CardSummary& saved) {
        if (entry.id != saved.entry.id || entry.revision >= saved.entry.revision) return;
        auto placement = entry;
        placement.snapshot_id = saved.entry.snapshot_id;
        placement.revision = saved.entry.revision;
        auto appearance = style;
        auto size = scale;
        *this = saved;
        entry = std::move(placement);
        style = appearance;
        scale = size;
    }
};
// Take only command identity and display values into the popup's nested message loop.
struct CardMenuSnapshot {
    domain::Entry entry;
    CardStyle style;
    CardMenuKind menu;
    bool unclassified_path;
    domain::PathKind observed_path_kind;
    std::optional<std::pair<size_t, size_t>> position;
    explicit CardMenuSnapshot(const CardSummary& card, std::optional<std::pair<size_t, size_t>> number = {})
        : entry(card.entry), style(card.style), menu(card.menu), unclassified_path(card.unclassified_path),
          observed_path_kind(card.observed_path_kind), position(number) {}
};
enum class DetailText {
    literal, capture_details, saved, source, capture, file_health, original_path, file_status, preview,
    inventory, partial_inventory, incomplete_inventory, local_time, unknown, missing, inaccessible,
    refresh, unchecked, available, not_applicable, decode_failed, ready, cached, unavailable,
    capture_status, format_status, windows_format, card_gone, information_unavailable, path
};
struct DetailLine {
    enum Kind { field, heading, format, note } kind;
    std::wstring label, value;
    DetailText label_text = DetailText::literal, value_text = DetailText::literal;
    int stream_index = -1;
    bool operator==(const DetailLine&) const = default;
};
using CardDetails = std::vector<DetailLine>;

struct Anchor {
    domain::Id entry;
    double offset{};
};
} // namespace cliprope::application
