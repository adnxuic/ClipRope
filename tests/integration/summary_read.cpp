#include "presentation/card_labels.hpp"
#include "history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"

#include <cstring>
#include <iostream>
#include <type_traits>

namespace {
template<class T> concept SavesAnchor = requires { &T::save_anchor; };
template<class T> concept SavesPathKind = requires { &T::save_path_kind; };
template<class T> concept ConfirmsMenu = requires { &T::confirm_menu_kind; };
using cliprope::persistence::Browser;
using cliprope::rendering::CardBrowser;
static_assert(!std::is_base_of_v<Browser, CardBrowser>);
static_assert(!SavesAnchor<Browser> && !SavesPathKind<Browser> && !ConfirmsMenu<Browser>);
static_assert(!SavesAnchor<CardBrowser> && !SavesPathKind<CardBrowser> && !ConfirmsMenu<CardBrowser>);
}

int main() {
    using namespace cliprope;
    using history_fixture::require;
    try {
        history_fixture::Directory fixture;
        persistence::HistoryStore store(fixture.path);
        persistence::Browser records(store);
        rendering::CardBrowser reader(records);
        const auto rope = store.active_rope();
        std::wstring text = L"First\r\nfield \u03a9 " + std::wstring(5000, L'Z');
        domain::Format format;
        format.standard_id = CF_UNICODETEXT;
        format.bytes.resize((text.size() + 1) * sizeof(wchar_t));
        std::memcpy(format.bytes.data(), text.c_str(), format.bytes.size());
        domain::Snapshot sample{{format}};
        const auto older = store.admit(rope, sample);
        const auto head = store.admit(rope, sample);
        auto page = reader.page(rope, 1, 1);
        require(page.size() == 1 && page[0].entry.id == older && page[0].text.starts_with(L"First field \u03a9 ") &&
                page[0].text.size() <= 128, "Large external Unicode preview was not bounded and normalized");
        store.save_anchor(rope, {older, 23.5});
        require(records.position(rope, older) == 1 && records.load_anchor(rope).offset == 23.5,
                "Saved anchor did not match the displayed order");
        store.promote(older);
        require(records.position(rope, older) == 0 && reader.page(rope, 1, 1)[0].entry.id == head,
                "Stable IDs were lost after promotion");
        store.detach(older);
        require(!records.position(rope, older) && records.count(rope) == 1 && reader.page(rope, 1, 1).empty(),
                "Floating entry was realized in the rope");
        // A Next() failure after the first saved format has no FORMATETC for
        // the missing descriptor. Capture persists the partial snapshot status alone.
        sample.capture_status = "partial";
        const auto interrupted = store.admit(rope, sample);
        const auto manifest = reader.manifest(interrupted);
        require(cliprope::presentation::english_card(reader.summary(interrupted)).capabilities == L"Text · partial" &&
                presentation::english_card(reader.page(rope, 0, 1).front()).capabilities == L"Text · partial" &&
                manifest.starts_with(L"Capture inventory · partial") &&
                manifest.find(L"CF_UNICODETEXT — saved") != manifest.npos &&
                manifest.find(L"source format list may be incomplete") != manifest.npos &&
                manifest.find(L"failed") == manifest.npos, "Interrupted inventory was presented as complete");
        require(cliprope::presentation::english_card(reader.summary(head)).capabilities == L"Text" &&
                reader.manifest(head).find(L"partial") == manifest.npos,
                "Complete capture was mislabeled as partial");
        const auto info = reader.information(cliprope::presentation::english_card(reader.summary(interrupted)));
        require(info.starts_with(L"Capture details\r\nSaved: ") && info.find(L"Source: Unknown") != info.npos &&
                info.find(L"Capture: partial") < info.find(L"Capture inventory · partial") &&
                info.find(L"File health") == info.npos && info.find(L"source format list may be incomplete") != info.npos,
                "Text information omitted metadata, completeness or showed file-only fields");
        sample.source_hint = "Synthetic source";
        sample.formats.push_back({0, "Synthetic skipped", {}, "skipped"});
        sample.formats.push_back({0, "Synthetic failed", {}, "failed"});
        const auto outcomes = store.admit(rope, sample);
        const auto current = cliprope::presentation::english_card(reader.summary(outcomes));
        const auto report = reader.information(current);
        require(report.find(L"Source: Synthetic source") != report.npos &&
                report.find(L"Synthetic skipped — skipped") != report.npos &&
                report.find(L"Synthetic failed — failed") != report.npos &&
                records.position(rope, outcomes) == 0 && store.entry(outcomes).revision == current.entry.revision,
                "Unified information lost format outcomes or changed the entry");
        store.erase(outcomes);
        history_fixture::rejects([&] { records.card_details(current); }, "Deleted card returned stale information");
        std::cout << "External text prefix, stable pages and offset anchor verified\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
