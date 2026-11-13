#pragma once
#include "presentation/card_capabilities.hpp"
#include "presentation/card_content.hpp"

namespace cliprope::presentation {
struct FileFacts {
    std::optional<FileList> list;
    std::string hash;
    domain::PathKind saved_kind{};
    bool physical{}, picture{};
};
inline void classify(application::CardSummary& card, const CardCapabilities& formats, const FileFacts& files) {
    using Kind = application::CardKind;
    using Menu = application::CardMenuKind;
    using Picture = application::CardSummary::FilePreviewKind;
    formats.apply(card);
    if (card.kind == Kind::text || card.kind == Kind::link) card.menu = Menu::text;
    else if (card.kind == Kind::image) card.menu = Menu::screenshot;
    if (files.hash.empty()) return;
    if (files.list && files.physical) {
        const bool single = files.list->count == 1 && !files.list->truncated;
        card.kind = Kind::files;
        card.image_primary = false;
        card.menu = single ? Menu::single_file : Menu::multiple_files;
        card.file_list_hash = files.hash;
        card.unclassified_path = single && files.saved_kind == domain::PathKind::unknown;
        if (single && files.saved_kind == domain::PathKind::directory) {
            card.kind = Kind::folder;
            card.menu = Menu::folder;
        } else if (files.picture && (!single || card.image_file_previews)) {
            card.file_preview = single ? Picture::picture : Picture::collage;
        }
    } else if (card.kind != Kind::text && card.kind != Kind::link) card.menu = Menu::other;
    if (files.list && (card.kind == Kind::files || card.kind == Kind::folder)) {
        card.text = leaf(files.list->first);
        card.generated_text = card.empty_text = false;
        card.location = parent(files.list->first);
        card.items = files.list->count;
        card.more_items = files.list->truncated;
    }
}
inline void apply_preview(application::CardSummary& card, domain::FilePreview preview) {
    if (card.unclassified_path && preview.directory) {
        card.observed_path_kind = *preview.directory ? domain::PathKind::directory : domain::PathKind::file;
        if (*preview.directory) {
            card.menu = application::CardMenuKind::folder;
            card.kind = application::CardKind::folder;
            card.file_preview = application::CardSummary::FilePreviewKind::none;
        }
    }
    card.source = preview.source;
    card.thumbnail = std::move(preview.image);
    card.availability = preview.availability;
}
} // namespace cliprope::presentation
