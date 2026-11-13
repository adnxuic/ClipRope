#include "rendering/card_browser.hpp"
#include "presentation/card_projection.hpp"
#include "platform_win/file_group_paths.hpp"
#include "platform_win/image_file_access.hpp"
#include "platform_win/media.hpp"
#include <cmath>
#include <limits>

namespace cliprope::rendering {
namespace {
using presentation::CardFormat;
std::optional<CardFormat> category(unsigned id, const std::string& name) {
    if (id == CF_UNICODETEXT)
        return CardFormat::text;
    if (name == "HTML Format")
        return CardFormat::html;
    if (name == "Rich Text Format")
        return CardFormat::rtf;
    if (id == CF_DIB || id == CF_DIBV5 || name == "PNG")
        return CardFormat::image;
    if (id == CF_HDROP)
        return CardFormat::file_references;
    if (name == "FileGroupDescriptorW" || name == "FileContents")
        return CardFormat::virtual_files;
    return {};
}
} // namespace

application::CardSummary CardBrowser::summary(std::string_view id, bool previews, unsigned width) {
    auto facts = records_.summary(id, previews ? persistence::FileListExtent::full : persistence::FileListExtent::prefix);
    return describe(facts, previews, width, nullptr);
}
std::vector<application::CardSummary> CardBrowser::page(std::string_view rope, size_t offset, size_t count,
                                                        bool previews, unsigned width, persistence::PageReadStats* stats) {
    auto rows = records_.page(rope, offset, count, previews ? persistence::FileListExtent::full : persistence::FileListExtent::prefix, stats);
    std::vector<application::CardSummary> result;
    result.reserve(rows.size());
    for (const auto& row : rows) result.push_back(describe(row, previews, width, stats));
    return result;
}
application::CardSummary CardBrowser::describe(const persistence::StoredCard& facts, bool with_preview,
                                              unsigned width, persistence::PageReadStats* stats) {
    application::CardSummary card;
    card.entry = facts.entry;
    card.style = facts.style;
    card.scale = facts.scale;
    card.text_unavailable = facts.text_unavailable;
    if (facts.text) card.text = presentation::text_preview(*facts.text, card.paper_text);
    card.hover_resolution = facts.hover_resolution;
    card.image_file_previews = facts.image_file_previews;
    presentation::CardCapabilities formats(facts.partial);
    formats.damaged = facts.damaged;
    for (const auto& [id, name] : facts.formats) {
        if (const auto type = category(id, name)) formats.retain(*type);
        else if (name != "Preferred DropEffect") formats.damaged = true;
    }
    presentation::FileFacts file;
    file.saved_kind = facts.path_kind;
    if (!facts.file_hash.empty()) {
        file.hash = facts.file_hash;
        file.physical = !facts.descriptors && facts.file_length <= max_group_bytes;
        file.list = facts.descriptors ? presentation::file_descriptors(facts.file_bytes) : presentation::drop_files(facts.file_bytes);
        if (file.list && file.physical && facts.file_bytes.size() == facts.file_length &&
            (card.image_file_previews || file.list->count != 1 || file.list->truncated))
            file.picture = image_file_group(file_group_paths(facts.file_bytes));
    }
    width = static_cast<unsigned>(std::clamp(std::round(double(width) * card.scale.width),
                                             1., double(std::numeric_limits<unsigned>::max())));
    card.image_preferences = image_preferences;
    presentation::classify(card, formats, file);
    if (card.image_primary && !facts.image_hash.empty()) {
        const auto& header = facts.image_header;
        if (header.size() >= 24) {
            auto word = [&](int at, bool big) {
                std::uint32_t v{};
                for (int i = 0; i < 4; ++i) v = (v << 8) | header[at + (big ? i : 3 - i)];
                return v;
            };
            const bool png = facts.image_format != CF_DIB && facts.image_format != CF_DIBV5;
            const auto w = word(png ? 16 : 4, png);
            auto h = word(png ? 20 : 8, png);
            if (!png && (h >> 31)) h = 0u - h;
            if (w && h && w <= media::max_side && h <= media::max_side && w <= media::max_pixels / h)
                card.source = {w, h};
        }
    }
    if (!with_preview || formats.damaged) return card;
    if (!card.file_list_hash.empty()) {
        const bool single = card.items == 1 && !card.more_items;
        if (stats) ++stats->preview_requests;
        auto preview = thumbnails_.request_files(card.file_list_hash, single ? width : std::min(width, rendering::preview_width),
                                                 card.file_preview != application::CardSummary::FilePreviewKind::none);
        presentation::apply_preview(card, std::move(preview));
    } else if (!facts.image_hash.empty()) {
        if (stats) ++stats->preview_requests;
        card.pixel_blob_id = facts.image_hash;
        card.thumbnail = thumbnails_.request(facts.image_hash, facts.image_format ? facts.image_format : RegisterClipboardFormatW(L"PNG"),
                                             card.image_primary ? width : std::min(width, rendering::preview_width));
    }
    return card;
}

application::CardSummary CardBrowser::menu_summary(std::string_view id) {
    return summary(id, true);
}

void CardBrowser::inspect_path_kind(application::CardSummary& card, PathLookup lookup) {
    if (!card.unclassified_path) return;
    auto kind = card.observed_path_kind;
    if (kind == domain::PathKind::unknown && lookup == PathLookup::inspect_for_menu) {
        const auto bytes = records_.file_list(card.file_list_hash);
        const auto paths = file_group_paths(bytes);
        if (paths.size() == 1) kind = win::local_path_kind(paths.front());
    }
    if (kind == domain::PathKind::unknown) return;
    card.observed_path_kind = kind;
    if (kind == domain::PathKind::directory) {
        card.menu = application::CardMenuKind::folder;
        card.kind = application::CardKind::folder;
        card.file_preview = application::CardSummary::FilePreviewKind::none;
    }
}

namespace {
std::wstring detail_text(const application::CardDetails& lines) {
    std::wstring text;
    for (const auto& line : lines) {
        if (!text.empty()) text += line.kind == application::DetailLine::heading ? L"\r\n\r\n" : L"\r\n";
        const auto [label, value] = presentation::detail_labels(line, presentation::english_labels());
        text += label;
        if (!value.empty()) text += (line.kind == application::DetailLine::format ? L" — " : L": ") + value;
    }
    return text;
}
}
std::wstring CardBrowser::information(const application::CardSummary& card) {
    return detail_text(records_.card_details(card));
}

std::wstring CardBrowser::manifest(std::string_view id) { return detail_text(records_.inventory_lines(id)); }

} // namespace cliprope::rendering
