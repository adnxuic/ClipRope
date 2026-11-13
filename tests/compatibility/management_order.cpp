#include "../integration/resident.hpp"
#include "../integration/history_fixture.hpp"
#include "../integration/history_observer.hpp"
#include "../clipboard_sources/sample.hpp"
#include "persistence/history_store.hpp"
#include "persistence/hotkey_settings.hpp"
#include "persistence/privacy.hpp"
#include "platform_win/performance_counters.hpp"
#include <algorithm>

namespace {
using namespace cliprope;
using probe::require;
using persistence::Statement;
DWORD_PTR send(HWND hwnd, UINT message, WPARAM w = 0, LPARAM l = 0) {
    DWORD_PTR result{};
    require(hwnd && SendMessageTimeoutW(hwnd, message, w, l, SMTO_ABORTIFHUNG, 500, &result),
            "Management control waited on the admission writer");
    return result;
}
int scalar(sqlite3* db, const char* query) {
    Statement row(db, query);
    require(row.step(), "Expected fixture row missing");
    return static_cast<int>(row.integer(0));
}

void settings_write(const wchar_t* exe, const wchar_t* source, bool reject, bool close_pending) {
    history_fixture::Directory dir;
    {
        persistence::HistoryStore seed(dir.path);
        persistence::save_card_preferences(seed.database(), {});
    }
    probe::Resident host(exe, dir.path, true);
    send(host.listener(), WM_COMMAND, 30); // Settings.
    const HWND settings = probe::owned_window(host.pid(), L"ClipRope.Settings");
    send(GetDlgItem(settings, 1501), BM_CLICK);
    const auto viewport = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
    const HWND follow = GetDlgItem(viewport, 1927);
    require(send(follow, BM_GETCHECK) == BST_CHECKED, "Committed head preference missing");
    auto db = history_fixture::observe(dir.path, SQLITE_OPEN_READWRITE);
    persistence::Transaction lock(db.get());
    // Only the order first capture -> setting -> younger external capture can pass.
    persistence::exec(db.get(), reject ?
        "CREATE TRIGGER order_guard BEFORE INSERT ON clipboard_follow_preferences BEGIN SELECT RAISE(ABORT,'rejected'); END" :
        "CREATE TRIGGER order_guard BEFORE INSERT ON clipboard_follow_preferences WHEN (SELECT count(*) FROM entries)<>1 "
        "BEGIN SELECT RAISE(ABORT,'wrong order'); END");
    probe::ExternalCopy first(source, L"normal");
    probe::pump_until([&] {
        return send(host.listener(), win::performance_query, WPARAM(win::PerformanceCounter::capture_writing)) != 0;
    }, "Capture never entered admission");
    send(follow, BM_CLICK);
    require(!IsWindowEnabled(viewport) && send(follow, BM_GETCHECK) == BST_CHECKED &&
            scalar(db.get(), "SELECT card_head FROM clipboard_follow_preferences") == 1 &&
            scalar(db.get(), "SELECT count(*) FROM entries") == 0, "Settings acknowledged an uncommitted write");
    if (close_pending) send(settings, WM_CLOSE);
    probe::ExternalCopy second(source, L"second");
    const auto sequence = GetClipboardSequenceNumber();
    const auto owner = GetClipboardOwner();
    lock.commit();
    probe::pump_until([&] {
        return scalar(db.get(), "SELECT count(*) FROM entries") == 2 && (close_pending || IsWindowEnabled(viewport));
    }, "Capture or management command did not finish");
    require(scalar(db.get(), "SELECT card_head FROM clipboard_follow_preferences") == int(reject) &&
            (close_pending || send(follow, BM_GETCHECK) == (reject ? BST_CHECKED : BST_UNCHECKED)) &&
            GetClipboardSequenceNumber() == sequence && GetClipboardOwner() == owner,
            "Pending preference escaped rollback, lost the write on close or overwrote the external copy");
    host.exit();
    persistence::HistoryStore reopened(dir.path);
    const auto entries = reopened.hanging("main");
    require(entries.size() == 2 && entries[0].id != entries[1].id &&
            persistence::read_card_preferences(reopened.database()).follow_card_head == reject,
            "Reopen lost management outcome or captures");
    for (unsigned i = 0; i < 2; ++i) {
        const auto content = reopened.load(entries[i].id);
        const auto bytes = sample::formats(i == 0).front().bytes;
        require(std::any_of(content.snapshot.formats.begin(), content.snapshot.formats.end(),
            [&](const auto& f) { return f.standard_id == CF_UNICODETEXT && f.bytes == bytes; }), "Wrong durable external bytes");
    }
    std::cout << "hold -> capture " << entries[1].id << " -> preference reject=" << reject << " closed=" << close_pending
              << " -> capture " << entries[0].id << " owner-preserved=1 sequence=" << sequence << '\n';
}

void exclusion_order(const wchar_t* exe, const wchar_t* source) {
    history_fixture::Directory dir;
    probe::Resident host(exe, dir.path, true);
    send(host.listener(), WM_COMMAND, 30);
    const HWND settings = probe::owned_window(host.pid(), L"ClipRope.Settings");
    send(GetDlgItem(settings, 1505), BM_CLICK);
    const auto viewport = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
    require(PostMessageW(GetDlgItem(viewport, 1944), BM_CLICK, 0, 0), "Cannot open exclusions");
    HWND dialog{};
    probe::pump_until([&] {
        dialog = probe::owned_window(host.pid(), L"#32770");
        return dialog && GetDlgItem(dialog, 1977);
    }, "Exclusions editor did not open");
    const auto name = std::filesystem::path(source).filename().wstring();
    send(GetDlgItem(dialog, 1972), WM_SETTEXT, 0, LPARAM(name.c_str()));
    send(GetDlgItem(dialog, IDOK), BM_CLICK);
    auto db = history_fixture::observe(dir.path, SQLITE_OPEN_READWRITE);
    persistence::Transaction lock(db.get());
    probe::ExternalCopy before(source, L"normal");
    using C = win::PerformanceCounter;
    auto counter = [&](C metric) { return send(host.listener(), win::performance_query, WPARAM(metric)); };
    probe::pump_until([&] { return counter(C::capture_writing) != 0; }, "Capture never held admission");
    send(GetDlgItem(dialog, 1977), BM_CLICK);
    require(IsWindow(dialog) && !IsWindowEnabled(GetDlgItem(dialog, 1977)), "Exclusions reported success before commit");
    // This source signals 'reading' if either Unicode or HTML GetData is called.
    probe::ExternalCopy excluded(source, L"paused");
    const auto sequence = GetClipboardSequenceNumber();
    probe::pump_until([&] { return counter(C::clipboard_sequence) == sequence; }, "Younger notification was not queued");
    lock.commit();
    probe::pump_until([&] {
        require(WaitForSingleObject(excluded.reading, 0) == WAIT_TIMEOUT, "Ordered exclusion allowed a payload read");
        return !IsWindow(dialog) && counter(C::capture_observed_sequence) == sequence &&
               win::capture_status_name(static_cast<DWORD>(counter(C::capture_observation))) == "excluded-app";
    }, "New exclusion did not govern younger observation");
    require(GetClipboardSequenceNumber() == sequence && scalar(db.get(), "SELECT count(*) FROM entries") == 1,
            "Excluded publication entered history or was replaced");
    host.exit();
    persistence::HistoryStore reopened(dir.path);
    require(reopened.hanging("main").size() == 1 &&
            persistence::read_privacy(reopened.database()).excluded_apps == std::vector{std::filesystem::path(name).string()},
            "Reopen lost the ordered capture or committed exclusion");
    std::cout << "capture -> exclusions Save -> younger source excluded; Unicode/HTML payload reads=0 entries=1\n";
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || !probe::private_station() || FAILED(OleInitialize(nullptr))) return 2;
    int code{};
    try {
        settings_write(argv[1], argv[2], false, false);
        settings_write(argv[1], argv[2], true, false);
        settings_write(argv[1], argv[2], false, true);
        exclusion_order(argv[1], argv[2]);
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; code = 1; }
    OleUninitialize();
    return code;
}
