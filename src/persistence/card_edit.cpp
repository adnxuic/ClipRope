#include "persistence/history_store.hpp"
#include "persistence/color_store.hpp"
#include <algorithm>
#include <cmath>

namespace cliprope::persistence {
namespace {
constexpr std::uint32_t unicode_text = 13;
bool valid_text(std::wstring_view text) {
    if (text.size() > application::max_edit_units)
        return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto unit = static_cast<std::uint16_t>(text[i]);
        if (!unit || (unit >= 0xdc00 && unit <= 0xdfff))
            return false;
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff)
                return false;
        }
    }
    return true;
}
domain::Snapshot plain_text(std::wstring_view text) {
    domain::Format value;
    value.standard_id = unicode_text;
    value.bytes.reserve((text.size() + 1) * 2);
    for (const auto unit : text) {
        value.bytes.push_back(static_cast<unsigned char>(unit & 255));
        value.bytes.push_back(static_cast<unsigned char>(unit >> 8));
    }
    value.bytes.insert(value.bytes.end(), {0, 0});
    domain::Snapshot snapshot;
    snapshot.capture_status = "edited";
    snapshot.formats.push_back(std::move(value));
    return snapshot;
}
} // namespace

domain::Id HistoryStore::create_floating_text(const domain::Layout& layout) {
    auto id = prepare_note_id();
    create_floating_text(id, layout);
    return id;
}
void HistoryStore::create_floating_text(const domain::Id& id, const domain::Layout& layout) {
    mutate([&] {
        const auto snapshot = content_.install(plain_text(L""));
        Statement add(db_.handle(), "INSERT INTO entries(id,snapshot_id,state) "
                                    "VALUES(?,?,'Floating')");
        add.bind(1, id);
        add.bind(2, snapshot);
        add.run();
        write_floating_layout(id, layout);
    });
}
application::TextDraft HistoryStore::begin_text_edit(std::string_view id) {
    static_assert(sizeof(wchar_t) == 2);
    std::lock_guard lock(db_.mutex());
    const auto card = entry(id);
    const auto saved = content_.load(card.snapshot_id);
    application::TextDraft draft{card.id, card.revision, {}, false};
    bool found = false;
    for (const auto &format : saved.snapshot.formats) {
        if (format.outcome != "saved")
            continue;
        if (format.standard_id != unicode_text || format.index != -1 || format.tymed != 1 ||
            format.aspect != 1) {
            draft.rich = true;
            continue;
        }
        const auto &bytes = format.bytes;
        if (found || bytes.size() < 2 || bytes.size() % 2 ||
            bytes.size() / 2 - 1 > application::max_edit_units || bytes[bytes.size() - 2] || bytes.back())
            throw std::runtime_error("Invalid editable Unicode representation");
        draft.text.reserve(bytes.size() / 2 - 1);
        for (size_t i = 0; i < bytes.size() - 2; i += 2)
            draft.text.push_back(static_cast<wchar_t>(bytes[i] | (static_cast<unsigned>(bytes[i + 1]) << 8)));
        if (!valid_text(draft.text))
            throw std::runtime_error("Invalid editable Unicode representation");
        found = true;
    }
    if (!found)
        throw std::runtime_error("This card has no saved Unicode text to edit");
    return draft;
}
domain::Id HistoryStore::save_text(std::string_view id, std::int64_t expected, std::wstring_view text) {
    if (!valid_text(text))
        throw std::invalid_argument("Text exceeds the limit or contains invalid UTF-16");
    const auto edited = plain_text(text);
    domain::Id snapshot;
    mutate_head([&] {
        Statement version(db_.handle(), "SELECT revision FROM entries WHERE id=?");
        version.bind(1, id);
        if (!version.step()) throw application::StaleEdit{true};
        if (version.integer(0) != expected) throw application::StaleEdit{};
        snapshot = content_.install(edited);
        Statement update(db_.handle(),
                         "UPDATE entries SET snapshot_id=?,revision=revision+1 WHERE id=? AND revision=?");
        update.bind(1, snapshot);
        update.bind(2, id);
        update.bind(3, expected);
        update.run();
        if (sqlite3_changes(db_.handle()) != 1)
            throw application::StaleEdit{};
    }, true);
    return snapshot;
}
void HistoryStore::save_style(std::string_view id, const application::CardStyle &style, bool set_font) {
    if (!std::isfinite(style.font_size) || style.font_size < 6 || style.font_size > 96 ||
        !std::isfinite(style.opacity) || style.opacity < 0 || style.opacity > 1 ||
        (style.text_color.custom && style.text_color.custom->a != 255))
        throw std::invalid_argument("Invalid card appearance");
    mutate([&] {
        Statement update(db_.handle(),
                         "UPDATE card_presentation SET "
                         "font_size=CASE WHEN ?7 THEN ?1 ELSE font_size END,font_override=font_override OR ?7,"
                         "bold=?2,text_ink=?3,paper_tint=?4,opacity=?5 WHERE entry_id=?6");
        update.bind(7, int(set_font));
        update.bind(1, style.font_size);
        update.bind(2, style.bold ? 1 : 0);
        update.bind(3, style.text_color.encode());
        update.bind(4, style.background_color.encode());
        update.bind(5, style.opacity);
        update.bind(6, id);
        update.run();
        if (sqlite3_changes(db_.handle()) != 1)
            throw application::StaleEdit{};
    });
}
void HistoryStore::save_color(const domain::Entry &expected, bool paper, domain::ColorSelection original,
                              domain::ColorSelection color) {
    if (!paper && color.custom && color.custom->a != 255)
        throw std::invalid_argument("Ink must be opaque");
    mutate([&] {
        const auto current = entry(expected.id);
        if (current.snapshot_id != expected.snapshot_id || current.revision != expected.revision)
            throw application::StaleEdit{};
        Statement write(db_.handle(),
                        paper ? "UPDATE card_presentation SET paper_tint=? WHERE entry_id=? AND paper_tint=?"
                              : "UPDATE card_presentation SET text_ink=? WHERE entry_id=? AND text_ink=?");
        write.bind(1, color.encode());
        write.bind(2, expected.id);
        write.bind(3, original.encode());
        write.run();
        if (sqlite3_changes(db_.handle()) != 1)
            throw application::StaleEdit{};
        if (color != original)
            remember_color(db_.handle(), color);
    });
}
} // namespace cliprope::persistence
