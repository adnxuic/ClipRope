#include "history_fixture.hpp"
#include "clipboard_process.hpp"
#include "../clipboard_sources/sample.hpp"
#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"
#include "platform_win/history_clipboard.hpp"
#include <barrier>
#include <iostream>
#include <thread>

namespace {
using namespace cliprope;
using history_fixture::rejects;
using history_fixture::require;
using persistence::HistoryStore;
using persistence::Statement;
namespace fs = std::filesystem;
domain::Snapshot rich() {
    auto text_html = sample::formats(false);
    domain::Snapshot snapshot;
    domain::Format text;
    text.standard_id = CF_UNICODETEXT;
    text.bytes = text_html[0].bytes;
    domain::Format html;
    html.name = "HTML Format";
    html.bytes = text_html[1].bytes;
    domain::Format rtf;
    rtf.name = "Rich Text Format";
    rtf.bytes = {'{', 'r', 't', 'f', '}'};
    domain::Format pixels;
    pixels.standard_id = CF_DIB;
    pixels.bytes = {0, 1, 2, 3};
    domain::Format private_data;
    private_data.name = "Synthetic Office Object";
    private_data.bytes = {5, 6, 7};
    snapshot.formats = {text, html, rtf, pixels, private_data};
    return snapshot;
}
std::vector<std::string> hashes(HistoryStore &store, const std::string &id) {
    Statement st(store.database().handle(),
                 "SELECT s.manifest_hash,r.blob_id FROM entries e JOIN snapshots s ON e.snapshot_id=s.id "
                 "JOIN representations r ON r.snapshot_id=s.id WHERE e.id=? ORDER BY r.ordinal");
    st.bind(1, id);
    std::vector<std::string> hashes;
    while (st.step()) {
        if (hashes.empty())
            hashes.push_back(st.text(0));
        hashes.push_back(st.text(1));
    }
    return hashes;
}
void identical(const domain::Snapshot &saved, const domain::Snapshot &expected) {
    require(saved.formats.size() == expected.formats.size(), "Original rich manifest count changed");
    for (size_t i = 0; i < expected.formats.size(); ++i) {
        const auto &a = saved.formats[i];
        const auto &b = expected.formats[i];
        require(a.bytes == b.bytes && a.standard_id == b.standard_id && a.name == b.name,
                "Original representation changed");
    }
}
void publish_plain(const domain::LeasedSnapshot &saved) {
    require(probe::private_station(), "Publication test requires private station");
    win::HistoryClipboard clipboard;
    require(clipboard.publish(saved) && clipboard.owns_current(), "Edited snapshot publication failed");
    struct Clear {
        win::HistoryClipboard &c;
        ~Clear() {
            if (c.owns_current())
                OleSetClipboard(nullptr);
        }
    } cleanup{clipboard};
    require(IsClipboardFormatAvailable(CF_UNICODETEXT) != FALSE && !IsClipboardFormatAvailable(CF_DIB) &&
                !IsClipboardFormatAvailable(RegisterClipboardFormatW(L"HTML Format")) &&
                !IsClipboardFormatAvailable(RegisterClipboardFormatW(L"Rich Text Format")) &&
                !IsClipboardFormatAvailable(RegisterClipboardFormatW(L"Synthetic Office Object")),
            "Edited publication still offers stale rich formats");
    IDataObject *object{};
    require(SUCCEEDED(OleGetClipboard(&object)), "Could not obtain edited publication");
    FORMATETC format{CF_UNICODETEXT, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM medium{};
    auto hr = object->GetData(&format, &medium);
    object->Release();
    require(SUCCEEDED(hr), "Advertised edited text unavailable");
    const auto &bytes = saved.snapshot.formats.front().bytes;
    auto *ptr = GlobalLock(medium.hGlobal);
    const bool valid =
        ptr && GlobalSize(medium.hGlobal) >= bytes.size() && !std::memcmp(ptr, bytes.data(), bytes.size());
    if (ptr)
        GlobalUnlock(medium.hGlobal);
    ReleaseStgMedium(&medium);
    require(valid, "Published bytes differ from edited snapshot");
}
void resize_storage(const fs::path& path) {
    domain::Id card, copy;
    const domain::Layout placement{"panel", 18, 22, 360, 160};
    {
        HistoryStore store(path);
        card = store.admit("main", rich());
        const auto hanging = store.entry(card);
        store.resize(hanging, {1.5f, 1});
        copy = store.copy(card, "main", &hanging);
        store.detach(card, placement);
        rejects([&] { store.resize(hanging, {2, 2}); }, "Stale Hanging resize accepted");
        const auto floating = store.entry(card);
        auto invalid = placement;
        invalid.width = 0;
        rejects([&] { store.resize(floating, {2, 2}, &invalid); }, "Invalid resize placement committed");
        rendering::CardBrowser browser{persistence::Browser(store)};
        require(browser.summary(card).scale == domain::CardScale{1.5f, 1} &&
                store.layout(card).width == placement.width, "Partial Floating resize survived rollback");
        auto changed = placement;
        changed.height = 120;
        store.resize(floating, {1.5f, .75f}, &changed);
        rejects([&] { store.resize(store.entry(copy), {2.01f, 1}); }, "Out-of-range resize accepted");
    }
    HistoryStore store(path);
    rendering::CardBrowser browser{persistence::Browser(store)};
    require(store.layout(card).height == 120 && browser.summary(card).scale == domain::CardScale{1.5f, .75f}
            && browser.summary(copy).scale == domain::CardScale{1.5f, 1}, "Reopen lost size or modified sibling");
    store.rehang(card, domain::rebin_id);
    require(browser.summary(card).scale == domain::CardScale{1.5f, .75f} &&
            store.hanging(domain::rebin_id).front().id == card, "ReBin lost shared scale");
}

void quick_note_storage(const fs::path& path) {
    const domain::Layout place{"display-2", 350, 270, 240, 160};
    domain::Id tail, note, blank;
    {
        HistoryStore store(path);
        store.set_capacity("main", 1);
        tail = store.admit("main", rich());
        auto snapshot_count = [&] {
            Statement count(store.database().handle(), "SELECT count(*) FROM snapshots");
            require(count.step(), "Missing snapshot count");
            return count.integer(0);
        };
        const auto snapshots = snapshot_count();
        auto invalid = place;
        invalid.height = 0;
        rejects([&] { store.create_floating_text(invalid); }, "Invalid placement was accepted");
        require(store.floating().empty() &&
                snapshot_count() == snapshots,
                "Failed creation left part of the new note");
        note = store.create_floating_text(place);
        blank = store.create_floating_text(place);
        const auto draft = store.begin_text_edit(note);
        require(draft.text.empty() && !draft.rich && note != blank, "New notes were not independent blank text");
        store.save_text(note, draft.revision, L"记录\r\nsecond line");
    }
    HistoryStore reopened(path);
    const auto entry = reopened.entry(note);
    const auto placed = reopened.layout(note);
    const auto summary = rendering::CardBrowser(persistence::Browser(reopened)).summary(blank);
    require(summary.menu == application::CardMenuKind::text && summary.paper_text && summary.paper_text->empty(),
            "Blank card paper displayed a menu placeholder");
    require(reopened.floating().size() == 2 && !entry.rope_id && !entry.sort_key &&
            reopened.begin_text_edit(note).text == L"记录\r\nsecond line" && reopened.begin_text_edit(blank).text.empty(),
            "Quick note identities or contents did not survive reopen");
    require(placed.monitor == place.monitor && placed.x == place.x && placed.y == place.y &&
            placed.width == place.width && placed.height == place.height && placed.visible,
            "Quick note layout did not survive reopen");
    const auto rope = reopened.hanging("main");
    require(rope.size() == 1 && rope.front().id == tail && reopened.active_rope() == "main",
            "Creating a note evicted or reordered a full rope");
}
void run(const fs::path &path, bool publish) {
    const auto source = rich();
    const application::CardStyle changed{21, true, 0x164355, 0xe3c481, .68};
    std::string original, edited, old_snapshot;
    std::vector<std::string> original_hashes;
    const std::wstring new_text = L"Edited \U0001f9f5\r\nsecond line";
    {
        HistoryStore store(path);
        original = store.admit("main", source);
        edited = store.copy(original, "main");
        store.detach(edited, {"synthetic", 30, 40, 240, 160});
        old_snapshot = store.entry(original).snapshot_id;
        require(store.entry(edited).snapshot_id == old_snapshot, "Copy is not shared");
        original_hashes = hashes(store, edited);
        require(original_hashes.size() == source.formats.size() + 1, "Missing fixture manifest digest");
        store.save_style(edited, changed);
        require(hashes(store, edited) == original_hashes && hashes(store, original) == original_hashes,
                "A style save changed content hashes");
        auto draft = store.begin_text_edit(edited);
        require(draft.text == sample::text(false) && draft.rich, "Editor did not expose rich downgrade");
        auto old = store.load(edited);
        store.save_text(edited, draft.revision, new_text);
        require(store.entry(edited).snapshot_id != old_snapshot &&
                    store.entry(edited).revision == draft.revision + 1 && !store.entry(edited).rope_id,
                "Content save lost revision or Floating identity");
        identical(old.snapshot, source);
        identical(store.load(original).snapshot, source);
        require(hashes(store, original) == original_hashes, "Copy-on-write changed sibling hashes");
    }
    HistoryStore store(path);
    rendering::CardBrowser reader{persistence::Browser(store)};
    require(reader.summary(edited).style == changed &&
                reader.summary(original).style == application::CardStyle{},
            "Appearance did not survive restart independently");
    require(store.entry(original).snapshot_id == old_snapshot && hashes(store, original) == original_hashes,
            "Original hashes changed on reopen");
    identical(store.load(original).snapshot, source);
    const auto current = store.begin_text_edit(edited);
    require(!current.rich && current.text == new_text, "Edited Unicode did not survive reopen");
    const auto plain = store.load(edited);
    require(plain.snapshot.formats.size() == 1 && plain.snapshot.formats[0].standard_id == CF_UNICODETEXT,
            "Stale representation survived edit");
    if (publish)
        publish_plain(plain);
    const std::wstring invalid(1, static_cast<wchar_t>(0xd800));
    rejects([&] { store.save_text(edited, current.revision, invalid); }, "Accepted an isolated surrogate");
    require(store.entry(edited).revision == current.revision, "Rejected input mutated the entry");

    std::barrier start(3);
    bool won[2]{};
    std::exception_ptr error[2];
    auto race = [&](int index) {
        start.arrive_and_wait();
        try {
            store.save_text(edited, current.revision, index ? L"writer B" : L"writer A");
            won[index] = true;
        } catch (const application::StaleEdit &) {
        } catch (...) {
            error[index] = std::current_exception();
        }
    };
    std::jthread first(race, 0), second(race, 1);
    start.arrive_and_wait();
    first.join();
    second.join();
    for (auto failure : error)
        if (failure)
            std::rethrow_exception(failure);
    require(won[0] != won[1] && store.begin_text_edit(edited).text == (won[0] ? L"writer A" : L"writer B"),
            "A stale concurrent editor overwrote the winner");
    auto draft = store.begin_text_edit(edited);
    store.erase(edited);
    bool lost = false;
    try {
        store.save_text(edited, draft.revision, L"late");
    } catch (const application::StaleEdit &) {
        lost = true;
    }
    require(lost && store.floating().empty(), "Delayed edit resurrected a deleted card");
    store.gc();
    identical(store.load(original).snapshot, source);
    std::cout << "independent_style=1 original_formats=5 edited_formats=1 winning_writers=1 "
                 "deleted_resurrections=0 published="
              << publish << "\n";
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    const bool publish = argc == 2 && std::wstring_view(argv[1]) == L"--publish";
    if (argc != 1 && !publish)
        return 2;
    if (publish && FAILED(OleInitialize(nullptr)))
        return 1;
    int status = 1;
    try {
        history_fixture::Directory tmp;
        {
            HistoryStore store(tmp.path / "font");
            rendering::CardBrowser browser{persistence::Browser(store)};
            const auto inherited = store.admit("main", rich());
            const auto fixed = store.copy(inherited, "main");
            auto style = browser.summary(inherited).style;
            style.bold = true;
            store.save_style(inherited, style, false);
            store.save_style(fixed, style); // Explicitly choosing even 14 DIP fixes this entry.
            persistence::exec(store.database().handle(), "UPDATE app_settings SET text_font_size=20.5");
            require(browser.summary(inherited).style.font_size == 20.5 && browser.summary(fixed).style.font_size == 14 &&
                    browser.summary(store.copy(inherited, "main")).style.font_size == 20.5 &&
                    browser.summary(store.copy(fixed, "main")).style.font_size == 14,
                    "Font inheritance was lost on a non-font change or copy");
        }
        resize_storage(tmp.path / "resize");
        quick_note_storage(tmp.path / "quick-note");
        run(tmp.path, publish);
        status = 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
    }
    if (publish)
        OleUninitialize();
    return status;
}
