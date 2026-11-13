#include "../clipboard_sources/image_file_sample.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "history_observer.hpp"
#include "clipboard_process.hpp"
#include "../compatibility/management_frame.hpp"
#include "platform_win/card_editor.hpp"
#include "platform_win/popup_menu.hpp"
#include "platform_win/localization.hpp"
#include "persistence/history_store.hpp"
#include "host/menu_observation.hpp"
#include "platform_win/card_files.hpp"
#include "platform_win/history_clipboard.hpp"
#include "platform_win/screenshot_folder.hpp"
#include <fstream>
#include <iostream>

namespace {
using namespace cliprope;
namespace fs = std::filesystem;
using history_fixture::require;
using Kind = application::CardMenuKind;
domain::Snapshot snapshot(std::vector<Representation> values) {
    CaptureResult capture;
    capture.outcome = "captured";
    for (const auto& f : values) capture.formats.push_back({f.format, f.tymed, f.aspect, f.index, "saved"});
    capture.saved = std::move(values);
    return win::capture_snapshot(std::move(capture));
}
void write(const fs::path& path, const std::vector<unsigned char>& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    require(stream.good(), "Could not write fixture");
}
std::string utf8(const fs::path& path) {
    const auto s = path.u8string();
    return {s.begin(), s.end()};
}
std::int64_t now() {
    FILETIME t{};
    GetSystemTimePreciseAsFileTime(&t);
    return (std::int64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
}
void check_information(persistence::Browser& records, rendering::CardBrowser& browser, const application::CardSummary& card,
                       const std::vector<fs::path>& paths) {
    const auto details = records.card_details(card);
    std::vector<std::wstring> actual;
    for (const auto& row : details)
        if (row.label_text == application::DetailText::original_path) actual.push_back(row.value);
    require(actual.size() == paths.size(), "Card information lost a path or added a pixel-card path");
    for (size_t i = 0; i < paths.size(); ++i)
        require(actual[i] == paths[i].wstring(), "Card information changed path order or duplicate members");
    const auto report = browser.information(card);
    require(report.starts_with(L"Capture details\r\nSaved: ") &&
            report.find(L"Source: ") != report.npos && report.find(L"Capture: captured") != report.npos,
            "Card information omitted capture metadata");
    const auto health = report.find(L"File health"), inventory = report.find(L"Capture inventory");
    require(inventory != report.npos, "Card information omitted the format inventory");
    require(paths.empty() ? health == report.npos : health < inventory && report.find(L"File status: ") < inventory,
            "Card information sections are missing or out of order");
}
void exercise() {
    history_fixture::Directory tmp;
    const auto profile = tmp.path / "profile", original = tmp.path / "snip.png";
    const auto png = media_sample::png();
    auto pixels = snapshot({png});
    domain::Id bound, managed, duplicate;
    fs::path derived;
    {
        persistence::HistoryStore store(profile);
        persistence::Browser records(store);
        rendering::CardBrowser browser(records);
        const auto item = tmp.path / "README", dir = tmp.path / "folder.png";
        fs::create_directory(dir);
        write(item, {1});
        const auto file = store.admit("main", snapshot({image_file_sample::drop({item})}));
        const auto folder = store.admit("main", snapshot({image_file_sample::drop({dir})}));
        auto legacy_directory = browser.summary(folder, true);
        for (int i = 0; i < 200 && legacy_directory.observed_path_kind == domain::PathKind::unknown; ++i) {
            Sleep(10);
            legacy_directory = browser.summary(folder, true);
        }
        host::confirm_menu_observation(store, browser, legacy_directory, rendering::CardBrowser::PathLookup::cached_preview);
        require(!legacy_directory.unclassified_path && records.saved_path_kind(legacy_directory.entry) == domain::PathKind::directory,
                "Preview worker did not resolve the legacy directory");
        auto stale = legacy_directory.entry;
        ++stale.revision;
        history_fixture::rejects([&] { records.saved_path_kind(stale); }, "Stale menu accepted a file-kind result");
        require(browser.menu_summary(file).menu == Kind::single_file && browser.menu_summary(folder).menu == Kind::folder,
                "Extensionless file or dotted folder misclassified");
        check_information(records, browser, browser.menu_summary(file), {item});
        check_information(records, browser, browser.menu_summary(folder), {dir});
        require(browser.information(browser.menu_summary(folder)).find(L"Preview: Not applicable") != std::wstring::npos,
                "Folder information claimed an image preview");
        fs::remove(dir);
        require(browser.menu_summary(folder).menu == Kind::folder, "Missing confirmed directory became a file");
        const auto mixed = store.admit("main", snapshot({image_file_sample::drop({item, dir})}));
        auto card = browser.menu_summary(mixed);
        require(card.menu == Kind::multiple_files, "Group misclassified");
        auto check = [&](const auto& id, domain::FileAvailability state) {
            browser.recheck_files(browser.menu_summary(id).file_list_hash);
            for (int i = 0; i != 200; ++i) {
                auto updated = browser.summary(id, true);
                if (updated.availability == state) return updated;
                Sleep(10);
            }
            throw std::runtime_error("File state did not update");
        };
        card = check(mixed, domain::FileAvailability::missing);
        check_information(records, browser, card, {item, dir});
        require(browser.information(card).find(L"Missing") != std::wstring::npos, "Missing member was overlooked");
        fs::create_directory(dir);
        check(mixed, domain::FileAvailability::available);
        const auto repeated_group = store.admit("main", snapshot({image_file_sample::drop({dir, item, dir})}));
        check_information(records, browser, browser.menu_summary(repeated_group), {dir, item, dir});
        auto pending = browser.menu_summary(file);
        pending.availability = domain::FileAvailability::on_request;
        require(browser.information(pending).find(L"File status: Refresh to check") != std::wstring::npos,
                "Unchecked file information claimed availability");
        pending.availability = domain::FileAvailability::inaccessible;
        check_information(records, browser, pending, {item});
        require(browser.information(pending).find(L"File status: Temporarily inaccessible") != std::wstring::npos,
                "Inaccessible file information lost its distinct state");
        const auto bad = tmp.path / "broken.png";
        write(bad, {0,1,2});
        const auto broken = store.admit("main", snapshot({image_file_sample::drop({bad})}));
        card = check(broken, domain::FileAvailability::decode_failed);
        check_information(records, browser, card, {bad});
        require(browser.information(card).find(L"File status: Available") != std::wstring::npos &&
                browser.information(card).find(L"Preview: Image could not be decoded") != std::wstring::npos,
                "Preview failure was confused with missing files or failed capture");
        require(browser.information(card).find(bad.wstring()) != std::wstring::npos, "Status omitted the original path");
        write(bad, png.bytes);
        check(broken, domain::FileAvailability::available);
        managed = store.admit("main", pixels);
        check_information(records, browser, browser.menu_summary(managed), {});
        duplicate = store.copy(managed, "main");
        require(browser.menu_summary(managed).menu == Kind::screenshot, "Pixel card misclassified");
        derived = win::screenshot_target(store, store.entry(managed), store.load(managed).snapshot, win::CardCommand::reveal).path;
        std::ifstream in(derived, std::ios::binary);
        require(std::vector<unsigned char>(std::istreambuf_iterator<char>(in), {}) == png.bytes, "Managed PNG differs");
        store.erase(managed);
        store.gc();
        require(fs::exists(derived), "Managed image lost while a copy still refers to it");
        pixels.snip_directory = utf8(tmp.path);
        pixels.snip_begin = now();
        Sleep(20);
        write(original, png.bytes);
        pixels.snip_end = now();
        bound = store.admit("main", pixels);
        require(win::bind_snip_original(store, store.entry(bound), pixels) == original, "Failed to bind the unique fresh PNG");
        write(tmp.path / "ambiguous.png", png.bytes);
        pixels.snip_end = now();
        const auto ambiguous = store.admit("main", pixels);
        require(win::bind_snip_original(store, store.entry(ambiguous), pixels).empty(), "Ambiguous original was bound");
        pixels.snip_begin = now();
        Sleep(20);
        const auto second = tmp.path / "second-snip.png";
        write(second, png.bytes);
        pixels.snip_end = now();
        const auto repeated = store.admit("main", pixels);
        require(win::bind_snip_original(store, store.entry(repeated), pixels) == second, "Identical snips crossed rounds");

        const auto native = tmp.path / "redirected-snips";
        fs::create_directory(native);
        auto legacy = snapshot({png});
        legacy.source_hint = "SnippingTool.exe";
        legacy.snip_directory = utf8(native);
        const auto legacy_id = store.admit("main", legacy);
        const auto entry = store.entry(legacy_id);
        const auto cached = win::screenshot_target(store, entry, legacy, win::CardCommand::open);
        auto reveal = [&] { return win::screenshot_target(store, entry, store.load(legacy_id).snapshot, win::CardCommand::reveal); };
        require(fs::is_regular_file(cached.path) && reveal().folder_only && reveal().path == native,
                "An existing managed copy displaced the native screenshots folder");
        auto different = png.bytes;
        different.back() ^= 1;
        write(native / "same-size.png", different);
        const auto recovered = native / "verified.png";
        write(recovered, png.bytes);
        const auto found = reveal();
        require(!found.folder_only && found.path == recovered && store.load(legacy_id).snapshot.screenshot_path == utf8(recovered),
                "An untracked system snip did not recover its byte-identical native PNG");
        const auto identical = store.admit("main", legacy);
        const auto uncertain = win::screenshot_target(store, store.entry(identical), legacy, win::CardCommand::reveal);
        require(uncertain.folder_only && uncertain.path == native, "Nearby identical captures shared an unverified original");
        auto old = snapshot({media_sample::dib(false)});
        old.source_hint = "SnippingTool.exe";
        const auto old_id = store.admit("main", old);
        const auto missing_metadata = win::screenshot_target(store, store.entry(old_id), old, win::CardCommand::reveal);
        require(missing_metadata.folder_only && missing_metadata.path == win::native_screenshot_folder(),
                "An old system screenshot without folder metadata did not reveal the Windows folder");
    }
    persistence::HistoryStore reopened(profile);
    fs::remove(original);
    require(win::screenshot_target(reopened, reopened.entry(bound), reopened.load(bound).snapshot, win::CardCommand::reveal).path == original,
            "Missing bound original silently changed paths");
    require(win::screenshot_target(reopened, reopened.entry(duplicate), reopened.load(duplicate).snapshot, win::CardCommand::open).path == derived,
            "Reopened managed PNG changed paths");
    const auto unrelated = derived.parent_path() / (std::string(32, 'a') + ".png");
    write(unrelated, png.bytes);
    reopened.erase(duplicate);
    reopened.gc();
    require(fs::exists(unrelated), "Collection removed an unowned file");
    require(!fs::exists(derived), "Last reference did not release the managed image");
}
// Keep a real popup open across the writer barrier while the underlying path disappears.
struct RetainedMenu {
    persistence::Transaction& writer;
    fs::path path, frames;
    bool held = true, saw_pending{}, refreshed{};
    const ULONGLONG deadline = GetTickCount64() + 5000;
    std::exception_ptr error;

    static void CALLBACK tick(HWND owner, UINT, UINT_PTR timer, DWORD) {
        auto& self = *reinterpret_cast<RetainedMenu*>(GetWindowLongPtrW(owner, GWLP_USERDATA));
        const auto menu = FindWindowW(L"ClipRope.CardMenu", nullptr);
        if (!menu) return;
        try {
            require(GetTickCount64() < self.deadline, "Retained popup did not deliver its refresh");
            if (!self.saw_pending) return;
            const auto open = GetDlgItem(menu, int(win::CardCommand::open));
            if (self.held) {
                require(!open, "Uncommitted file-kind exposed the Open command");
                management_fixture::frame(menu, self.frames / "retained-before.bmp");
                require(fs::remove(self.path), "Could not remove the observed path");
                self.writer.commit();
                self.held = false;
                win::PopupMenuHost::file_preview_ready();
                return;
            }
            if (!open) return;
            management_fixture::frame(menu, self.frames / "retained-after.bmp");
            self.refreshed = true;
        } catch (...) { self.error = std::current_exception(); }
        KillTimer(owner, timer);
        SendMessageW(menu, WM_CLOSE, 0, 0);
    }
};

void retained_menu(const fs::path& frames = {}) {
    require(!frames.empty() || probe::private_station(), "Retained popup requires fixture isolation");
    if (!frames.empty()) {
        fs::create_directories(frames);
        management_fixture::record_frames = true;
    }
    history_fixture::Directory temp;
    const auto profile = temp.path / "profile";
    const auto path = temp.path / "disappears";
    write(path, {1});
    persistence::HistoryStore store(profile);
    persistence::Browser records(store);
    rendering::CardBrowser browser(records);
    const auto id = store.admit("main", snapshot({image_file_sample::drop({path})}));
    auto other = history_fixture::observe(profile, SQLITE_OPEN_READWRITE);
    persistence::Transaction writer(other.get());
    auto card = browser.menu_summary(id);
    browser.inspect_path_kind(card, rendering::CardBrowser::PathLookup::inspect_for_menu);
    require(card.unclassified_path && card.observed_path_kind == domain::PathKind::file, "First menu lost the probed file");
    RetainedMenu state{writer, path, frames};
    win::EditorActions actions;
    actions.confirm_path_kind = [&](const application::CardMenuSnapshot& menu) {
        require(menu.entry.id == id && menu.observed_path_kind == domain::PathKind::file, "Popup lost the bound observation");
        if (!state.held) return store.confirm_menu_kind(menu);
        state.saw_pending = true;
        return records.saved_path_kind(menu.entry);
    };
    localization::LocalizationManager language;
    win::UiLocalization binding(language);
    const auto owner = CreateWindowW(L"STATIC", L"Menu fixture", WS_OVERLAPPEDWINDOW, 0, 0, 320, 200,
                                     nullptr, nullptr, nullptr, nullptr);
    require(owner != nullptr, "Could not create menu owner");
    struct Owner { HWND hwnd; ~Owner() { DestroyWindow(hwnd); } } cleanup{owner};
    ShowWindow(owner, SW_SHOWNOACTIVATE);
    SetWindowLongPtrW(owner, GWLP_USERDATA, LONG_PTR(&state));
    require(SetTimer(owner, 1, 40, RetainedMenu::tick), "Could not schedule popup driver");
    win::CardEditor editor(std::move(actions));
    editor.menu(owner, {40, 40}, application::CardMenuSnapshot(card));
    if (state.error) std::rethrow_exception(state.error);
    require(state.refreshed && !fs::exists(path) && records.saved_path_kind(card.entry) == domain::PathKind::file,
            "Retained popup did not expose its previously observed kind after the writer drained");
    if (!frames.empty()) {
        const auto before = persistence::file_sha256(frames / "retained-before.bmp");
        const auto after = persistence::file_sha256(frames / "retained-after.bmp");
        require(before != after, "Retained menu frames did not show a transition");
        std::cout << "retained-before.bmp sha256=" << before << "\nretained-after.bmp sha256=" << after << '\n';
    }
    std::cout << "Retained off-screen menu kept its bound file observation through writer release and path removal\n";
}

void projections() {
    using Lookup = rendering::CardBrowser::PathLookup;
    using Path = domain::PathKind;
    history_fixture::Directory tmp;
    const auto profile = tmp.path / "profile";
    const auto directory = tmp.path / "legacy.png", file = tmp.path / "legacy-file";
    fs::create_directory(directory);
    write(file, {1});
    domain::Id folder, ordinary;
    {
        persistence::HistoryStore store(profile);
        persistence::Browser records(store);
        rendering::CardBrowser browser(records);
        auto* db = store.database().handle();
        ordinary = store.admit("main", snapshot({image_file_sample::drop({file})}));
        folder = store.admit("main", snapshot({image_file_sample::drop({directory})}));
        auto unknown = browser.summary(folder, true);
        int writes{};
        auto readonly = [](void* context, int op, const char*, const char*, const char*, const char*) {
            if (op == SQLITE_UPDATE || op == SQLITE_DELETE || op == SQLITE_INSERT) {
                ++*static_cast<int*>(context);
                return SQLITE_DENY;
            }
            return SQLITE_OK;
        };
        require(sqlite3_set_authorizer(db, readonly, &writes) == SQLITE_OK, "Read-only authorizer failed");
        {
            struct Restore { sqlite3* db; ~Restore() { sqlite3_set_authorizer(db, nullptr, nullptr); } } restore{db};
            auto other_writer = history_fixture::observe(profile, SQLITE_OPEN_READWRITE);
            persistence::Transaction hold(other_writer.get());
            auto held_menu = browser.menu_summary(ordinary);
            browser.inspect_path_kind(held_menu, Lookup::inspect_for_menu);
            require(held_menu.observed_path_kind == Path::file && held_menu.unclassified_path &&
                    records.saved_path_kind(held_menu.entry) == Path::unknown && writes == 0,
                    "Menu discovery waited on, wrote through, or lost the held writer");
            hold.commit();
            for (int i = 0; i < 200 && unknown.observed_path_kind == Path::unknown; ++i) {
                Sleep(10);
                unknown = browser.summary(folder, true);
            }
            require(unknown.observed_path_kind == Path::directory && unknown.unclassified_path &&
                    records.saved_path_kind(unknown.entry) == Path::unknown &&
                    browser.menu_summary(ordinary).unclassified_path && browser.page("main", 0, 2, true).size() == 2,
                    "Pure projection changed durable classification");
            application::RopeChange clear;
            clear.op = application::RopeOp::clear;
            clear.rope = "main";
            require(store.preview_rope_change(clear).hanging == 2 && records.rope(domain::rebin_id).protected_rope(),
                    "Pure rope query lost an affected entry or a system rope");
        }
        require(!writes, "A query attempted a write");
        unsigned selects{};
        sqlite3_trace_v2(db, SQLITE_TRACE_STMT, [](unsigned, void* context, void* statement, void*) {
            const char* sql = sqlite3_sql(static_cast<sqlite3_stmt*>(statement));
            if (sql && std::string_view(sql).starts_with("SELECT")) ++*static_cast<unsigned*>(context);
            return 0;
        }, &selects);
        persistence::PageReadStats reads;
        const auto page = browser.page("main", 0, 2, true, 320, &reads);
        sqlite3_trace_v2(db, 0, nullptr, nullptr);
        const auto expected_bytes = image_file_sample::drop({file}).bytes.size() + image_file_sample::drop({directory}).bytes.size();
        require(page.size() == 2 && selects == 5 && reads.prefix_bytes == expected_bytes && reads.preview_requests == 2,
                "Visible page exceeded the bounded file projection workload");
        std::cout << "page_cards=2 selects=" << selects << " prefix_bytes=" << reads.prefix_bytes
                  << " thumbnail_requests=" << reads.preview_requests << '\n';
        persistence::exec(db, "CREATE TEMP TRIGGER reject_kind BEFORE UPDATE OF path_kind ON snapshots "
                              "BEGIN SELECT RAISE(ABORT,'injected kind failure'); END");
        history_fixture::rejects([&] { host::confirm_menu_observation(store, browser, unknown, Lookup::cached_preview); }, "Faulted confirmation committed");
        require(records.saved_path_kind(unknown.entry) == Path::unknown && unknown.unclassified_path, "Failed update changed its projection");
        persistence::exec(db, "DROP TRIGGER reject_kind");
        host::confirm_menu_observation(store, browser, unknown, Lookup::cached_preview);
        auto first_menu = browser.menu_summary(ordinary);
        host::confirm_menu_observation(store, browser, first_menu, Lookup::inspect_for_menu);
        require(!first_menu.unclassified_path && first_menu.menu == Kind::single_file && unknown.menu == Kind::folder &&
                records.saved_path_kind(first_menu.entry) == Path::file && records.saved_path_kind(unknown.entry) == Path::directory,
                "Menu/preview lost the confirmed path kind");

        auto competitor = browser.menu_summary(ordinary);
        competitor.unclassified_path = true;
        competitor.observed_path_kind = Path::directory;
        require(store.confirm_menu_kind(application::CardMenuSnapshot(competitor)) == Path::file,
                "Late path observation overrode a kind already confirmed for the entry");

        const auto doomed = store.admit("main", snapshot({image_file_sample::drop({directory})}));
        auto stale = browser.menu_summary(doomed);
        browser.inspect_path_kind(stale, Lookup::inspect_for_menu);
        require(stale.observed_path_kind == Path::directory && stale.menu == Kind::folder, "Lost the probed legacy folder");
        auto revised = stale;
        ++revised.entry.revision;
        history_fixture::rejects([&] { store.confirm_menu_kind(application::CardMenuSnapshot(revised)); },
                                 "A different revision accepted a pending path observation");
        require(records.saved_path_kind(stale.entry) == Path::unknown, "Wrong revision classified a live snapshot");
        store.erase(doomed);
        history_fixture::rejects([&] { store.save_path_kind(stale.entry, stale.observed_path_kind); },
                                 "A queued confirmation accepted an evicted entry");
        history_fixture::rejects([&] { host::confirm_menu_observation(store, browser, stale, Lookup::inspect_for_menu); }, "Classification accepted a deleted entry");
        persistence::Statement old(db, "SELECT path_kind FROM snapshots WHERE id=?");
        old.bind(1, stale.entry.snapshot_id);
        require(old.step() && old.integer(0) == int(Path::unknown), "Late classification mutated an unreachable snapshot");
        auto mixed = snapshot({image_file_sample::drop({file})});
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = {'x', 0, 0, 0};
        mixed.formats.push_back(text);
        const auto editable = store.admit("main", mixed);
        auto held = browser.menu_summary(editable);
        browser.inspect_path_kind(held, Lookup::inspect_for_menu);
        require(held.observed_path_kind == Path::file, "Menu failed to bind the original file observation");
        store.save_text(editable, held.entry.revision, L"Updated value");
        history_fixture::rejects([&] { store.confirm_menu_kind(application::CardMenuSnapshot(held)); },
                                 "A retained pre-edit observation classified the new snapshot");
        require(records.saved_path_kind(store.entry(editable)) == Path::unknown &&
                store.begin_text_edit(editable).text == L"Updated value", "Late observation altered the edited card");
    }
    fs::remove(directory);
    persistence::HistoryStore reopened(profile);
    persistence::Browser records(reopened);
    rendering::CardBrowser browser(records);
    require(browser.menu_summary(folder).menu == Kind::folder && records.saved_path_kind(reopened.entry(ordinary)) == Path::file,
            "Reopen lost a previously confirmed kind");
    std::cout << "Pure projected reads attempted zero writes; explicit confirmation, stale deletion and restart checked\n";
}
}
int main(int argc, char** argv) {
    const bool visual = argc == 3 && std::string_view(argv[1]) == "--retained-menu-frames";
    struct TestDesktop {
        HDESK previous = GetThreadDesktop(GetCurrentThreadId()), isolated{};
        ~TestDesktop() { if (isolated) { SetThreadDesktop(previous); CloseDesktop(isolated); } }
    } desktop;
    if (visual) {
        const auto name = L"ClipRope.RetainedMenu." + std::to_wstring(GetCurrentProcessId());
        desktop.isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desktop.isolated || !SetThreadDesktop(desktop.isolated)) return 2;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    }
    if (FAILED(OleInitialize(nullptr))) return 1;
    struct Ole { ~Ole() { OleUninitialize(); } } ole;
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--projections") projections();
        else if (visual) retained_menu(fs::absolute(argv[2]));
        else if (argc == 2 && std::string_view(argv[1]) == "--retained-menu") retained_menu();
        else if (argc == 1) exercise();
        else return 2;
        return 0;
    }
    catch (const std::exception& ex) { std::cerr << ex.what() << '\n'; return 1; }
}
