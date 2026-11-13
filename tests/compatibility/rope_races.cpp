#include "platform_win/menu_timing.hpp"
#include "platform_win/card_editor.hpp"
#include "platform_win/drag_gesture.hpp"
#include "platform_win/file_group.hpp"
#include "platform_win/workspace_commands.hpp"
#include "persistence/hotkey_settings.hpp"
#include "persistence/localization_settings.hpp"
#include "hidden_tray.hpp"
#include "persistence/privacy.hpp"
#include "platform_win/tray.hpp"
#include "../performance/order_fixture.hpp"
#include "desktop_app.hpp"
#include "../integration/history_observer.hpp"
#include "../integration/diagnostic_report.hpp"
#include "../clipboard_sources/external_copy.hpp"
#include <windowsx.h>

namespace {
using namespace workspace_fixture;

static_assert(win::begins_card_press(WM_LBUTTONDOWN, false));
static_assert(win::begins_card_press(WM_LBUTTONDOWN, true));
static_assert(win::begins_card_press(WM_LBUTTONDBLCLK, true));
static_assert(!win::begins_card_press(WM_LBUTTONDBLCLK, false));
static_assert(!win::begins_card_press(WM_LBUTTONUP, true));

struct Seed { domain::Id b, c, floating, tail_b, tail_c; };

Seed prepare(const fs::path& root) {
    HistoryStore store(root);
    Seed seed;
    seed.b = store.create_rope("Collector B", 2);
    seed.c = store.create_rope("Collector C", 2);
    auto text = sample::formats(false).front();
    domain::Format fmt;
    fmt.standard_id = CF_UNICODETEXT;
    fmt.bytes = text.bytes;
    domain::Snapshot snapshot{{fmt}};
    seed.floating = store.admit(seed.b, snapshot);
    store.detach(seed.floating, {"", 40, 710, 240, 160});
    seed.tail_b = store.admit(seed.b, snapshot);
    seed.tail_c = store.admit(seed.c, snapshot);
    store.set_active_rope(seed.b);
    return seed;
}

class Observation {
public:
    explicit Observation(const fs::path& root) {
        auto file = (root / "history.db").u8string();
        sqlite3* raw{};
        const auto rc = sqlite3_open_v2(reinterpret_cast<const char*>(file.c_str()), &raw, SQLITE_OPEN_READONLY, nullptr);
        handle_.reset(raw);
        require(rc == SQLITE_OK, "Could not observe the live history");
        sqlite3_busy_timeout(raw, 500);
    }
    domain::Id active() const {
        Statement row(handle_.get(), "SELECT active_rope_id FROM app_settings");
        require(row.step(), "Collector setting missing");
        return row.text(0);
    }
    Order ids(const domain::Id& rope) const {
        Statement row(handle_.get(), "SELECT id FROM entries WHERE rope_id=? ORDER BY sort_key,id");
        row.bind(1, rope);
        Order out;
        while (row.step()) out.push_back(row.text(0));
        return out;
    }
    bool exists(const domain::Id& rope) const {
        Statement row(handle_.get(), "SELECT 1 FROM ropes WHERE id=?");
        row.bind(1, rope);
        return row.step();
    }
private:
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> handle_{nullptr, sqlite3_close};
};

void pointer(HWND hwnd, UINT message, POINT screen, WPARAM buttons = 0) {
    ScreenToClient(hwnd, &screen);
    DWORD_PTR result{};
    require(SendMessageTimeoutW(hwnd, message, buttons, MAKELPARAM(screen.x, screen.y),
                                SMTO_ABORTIFHUNG, 2500, &result) != 0, "Native control did not handle pointer input");
}
void click(HWND hwnd, POINT p, UINT press = WM_LBUTTONDOWN) {
    pointer(hwnd, press, p, MK_LBUTTON);
    pointer(hwnd, WM_LBUTTONUP, p);
}
POINT at_dip(HWND hwnd, int x, int y) {
    const auto dpi = GetDpiForWindow(hwnd);
    POINT point{MulDiv(x, dpi, 96), MulDiv(y, dpi, 96)};
    ClientToScreen(hwnd, &point);
    return point;
}
RECT row(Desktop& host, const domain::Id& rope) {
    RECT r{};
    require(until([&] {
        for (const auto& [id, rect] : host.frame().rows) if (id == rope) { r = rect; return true; }
        return false;
    }), "Rope did not get a realized row");
    return r;
}
void overview(Desktop& host, const domain::Id& rope) {
    SendMessageW(host.workspace, WM_COMMAND, static_cast<WPARAM>(cliprope::win::WorkspaceCommand::overview), 0);
    row(host, rope);
}
void select(Desktop& host, const Observation& db, const domain::Id& id) {
    const auto point = center(row(host, id));
    const auto before = db.active();
    click(host.workspace, point);
    require(db.active() == before, "Short press changed collector");
    pointer(host.workspace, WM_LBUTTONDBLCLK, point, MK_LBUTTON);
    pointer(host.workspace, WM_LBUTTONUP, point);
    require(until([&] { return db.active() == id; }), "Double-click did not select its collector");
    overview(host, id);
    require(until([&] {
        const auto frame = host.frame();
        return frame.rows.size() > 1 && frame.rows.front().first == id;
    }), "Overview did not present the selected collector at its head");
}
void close_host(Desktop& host) {
    host.stop();
    require(host.clean_exit, "Desktop did not exit cleanly");
}

RECT hanging_bounds(Desktop& host, const domain::Id& id);

void observer_window(const fs::path& exe, const fs::path& profile, bool returning, bool rollback) {
    domain::Id f, a, b, snapshot;
    {
        HistoryStore seed(profile);
        seed.set_capacity("main", 2);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = sample::formats(false).front().bytes;
        domain::Snapshot value{{text}};
        if (returning) {
            f = seed.admit("main", value);
            seed.detach(f, {"", 40, 300, 240, 160});
            b = seed.admit("main", value);
            a = seed.admit("main", value);
        } else {
            a = seed.admit("main", value);
            f = seed.admit("main", value);
        }
        snapshot = seed.entry(f).snapshot_id;
    }
    const Order before = returning ? Order{a,b} : Order{f,a};
    const Order after = returning ? Order{f,a} : Order{a};
    const wchar_t* fault = rollback ? (returning ? L"return" : L"detach") : L"head-observer";
    {
        Desktop host(exe, profile, true, fault, false, true);
        Observation db(profile);
        if (returning) {
            auto card = host.floating_window();
            require(card != nullptr, "Return fixture did not restore Floating");
            click(card, at_dip(card, 40, 18));
        } else {
            const auto start = center(hanging_bounds(host, f));
            RECT workspace{};
            require(GetWindowRect(host.workspace, &workspace), "Missing workspace bounds");
            const POINT outside{start.x, workspace.bottom + 100};
            pointer(host.workspace, WM_LBUTTONDOWN, start, MK_LBUTTON);
            pointer(host.workspace, WM_MOUSEMOVE, outside, MK_LBUTTON);
            pointer(host.workspace, WM_LBUTTONUP, outside);
        }
        const auto expected = std::string(returning ? "return " : "detach ") + f + (rollback ? " failed" : " committed");
        require(until([&] { return host.contains(expected); }), "Missing desktop transaction outcome");
        require(db.ids("main") == (rollback ? before : after) &&
                bool(host.floating_window()) == (returning == rollback), "Window disagrees with committed history");
        require(host.contains("injected head observer failure") == !rollback, "Wrong observer boundary");
        close_host(host);
    }
    const auto report = history_fixture::diagnostic_report(profile);
    require(report.find(rollback ? "Post-commit notification failures: 0\r\n" :
                                   "Post-commit notification failures: 1\r\n") != report.npos,
            "Failed notification counted as a history rollback");
    HistoryStore reopened(profile);
    require(order(profile) == (rollback ? before : after) && reopened.entry(f).snapshot_id == snapshot,
            "Restart changed the transaction outcome");
    std::cout << (returning ? "return" : "detach") << (rollback ? " rolled back" : " committed despite notification failure")
              << "; window and restart membership agree\n";
}

RECT hanging_bounds(Desktop& host, const domain::Id& id) {
    // The private station's desktop is hidden. Use the workspace's presented
    // slots and its own WS_VISIBLE style to observe show/hide decisions.
    RECT rect{};
    require(until([&] {
        for (const auto& [entry, bounds] : host.frame().cards)
            if (entry == id) { rect = bounds; return true; }
        return false;
    }), "Hanging target was not presented");
    return rect;
}

POINT hanging_close_point(Desktop& host, const domain::Id& id) {
    const auto rect = hanging_bounds(host, id);
    const auto dpi = GetDpiForWindow(host.workspace);
    return {rect.right - MulDiv(13, dpi, 96), rect.top + MulDiv(12, dpi, 96)};
}

void close_hanging(Desktop& host, const domain::Id& id) {
    click(host.workspace, hanging_close_point(host, id));
}

POINT floating_close_point(HWND card) {
    RECT rect{};
    require(GetWindowRect(card, &rect) != FALSE, "Missing Floating card");
    const auto inset = MulDiv(19, GetDpiForWindow(card), 96);
    return {rect.right - inset, rect.top + inset};
}

void rapid_hanging_deletion(const fs::path& exe, const fs::path& path, bool in_overview) {
    domain::Id a, b, c, other_rope;
    {
        HistoryStore store(path);
        if (in_overview) other_rope = store.create_rope("Other", 3);
        auto prefs = read_card_preferences(store.database());
        prefs.recycle_hanging = in_overview;
        save_card_preferences(store.database(), prefs);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = sample::formats(false).front().bytes;
        c = store.admit("main", {{text}});
        b = store.admit("main", {{text}});
        a = store.admit("main", {{text}});
    }
    Desktop host(exe, path, true);
    if (in_overview) overview(host, other_rope);
    const auto point = hanging_close_point(host, a);
    click(host.workspace, point);
    click(host.workspace, point, WM_LBUTTONDBLCLK);
    const auto remaining = order(path);
    require(remaining != Order{b,c}, "Rapid close removed A but missed the second click on B");
    require(remaining == Order{c} && order(path, domain::rebin_id) == (in_overview ? Order{b,a} : Order{}),
            "Rapid close changed the wrong cards or ReBin order");

    pointer(host.workspace, WM_LBUTTONDBLCLK, point, MK_LBUTTON);
    GUITHREADINFO thread{sizeof(thread)};
    require(GetGUIThreadInfo(GetWindowThreadProcessId(host.workspace, nullptr), &thread) &&
            thread.hwndCapture == host.workspace, "Close press on C did not take capture");
    const auto offset = MulDiv(40, GetDpiForWindow(host.workspace), 96);
    const POINT body{point.x - offset, point.y + offset};
    pointer(host.workspace, WM_LBUTTONUP, body);
    require(order(path) == Order{c}, "Releasing outside the close button removed C");
    click(host.workspace, body);
    click(host.workspace, body, WM_LBUTTONDBLCLK);
    require(host.contains("invoke double-click " + c + " published ") &&
            !(GetWindowLongPtrW(host.workspace, GWL_STYLE) & WS_VISIBLE) && order(path) == Order{c},
            "A body double-click after rapid close did not invoke C and collapse");
    close_host(host);
}

void card_deletion(const fs::path& exe, const fs::path& path) {
    domain::Id ordinary, head, next, last, shared, floating, snapshot;
    domain::CardScale scale{1.1f, 1.f};
    application::CardStyle style;
    style.bold = true;
    {
        HistoryStore store(path);
        ordinary = store.create_rope("ReBin", 8);
        store.set_active_rope(ordinary);
        const auto sample_text = sample::formats(false).front();
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = sample_text.bytes;
        last = store.admit(ordinary, {{text}});
        next = store.admit(ordinary, {{text}});
        head = store.admit(ordinary, {{text}});
        snapshot = store.entry(head).snapshot_id;
        store.save_style(head, style);
        store.resize(store.entry(head), scale);
        shared = store.copy(head, "main");
        floating = store.copy(head, "main");
        store.detach(floating, {"", 40, 710, 240, 160});
    }
    {
        Desktop host(exe, path, true, L"recycle");
        DWORD pid{};
        GetWindowThreadProcessId(host.workspace, &pid);
        SendMessageW(owned_window(pid, L"ClipRope.Listener"), WM_COMMAND, 30, 0);
        const auto settings = owned_window(pid, L"ClipRope.Settings");
        require(settings != nullptr, "Settings did not open");
        SendDlgItemMessageW(settings, 1501, BM_CLICK, 0, 0);
        const auto view = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
        const auto hanging = GetDlgItem(view, 1929);
        const auto independent = GetDlgItem(view, 1930);
        require(hanging && independent && SendMessageW(hanging, CB_GETCURSEL, 0, 0) == 0 &&
                SendMessageW(independent, CB_GETCURSEL, 0, 0) == 1, "Wrong legacy deletion defaults");
        SendMessageW(hanging, WM_KEYDOWN, VK_DOWN, 0);
        SendMessageW(independent, WM_KEYDOWN, VK_UP, 0);
        SendMessageW(settings, WM_CLOSE, 0, 0);
        const auto before = GetClipboardSequenceNumber();
        const auto head_close = hanging_close_point(host, head);
        click(host.workspace, head_close);
        require(host.contains("recycle " + head + " failed") && order(path, ordinary) == Order{head,next,last} &&
                order(path, domain::rebin_id).empty() && GetClipboardSequenceNumber() == before,
                "Rejected recycle changed history or clipboard");
        click(host.workspace, head_close, WM_LBUTTONDBLCLK);
        require(order(path, ordinary) == Order{next,last} && order(path, domain::rebin_id) == Order{head} &&
                until([&] { return host.contains("follow-head published " + next); }) &&
                clipboard_text_is(sample::text(false)), "Head recycle did not publish the successor");
        overview(host, ordinary);
        const auto after = GetClipboardSequenceNumber();
        close_hanging(host, last);
        close_hanging(host, next);
        require(order(path, ordinary).empty() && order(path, domain::rebin_id) == Order{next,last,head} &&
                (GetWindowLongPtrW(host.workspace, GWL_STYLE) & WS_VISIBLE) && GetClipboardSequenceNumber() == after,
                "Overview recycle replaced the clipboard or lost ReBin order");
        close_host(host);
    }
    {
        HistoryStore store(path);
        const auto prefs = read_card_preferences(store.database());
        require(prefs.recycle_hanging && !prefs.recycle_floating && store.active_rope() == ordinary &&
                !store.rebin_visible() && store.entry(head).snapshot_id == snapshot &&
                Browser(store).summary(head).style == style && Browser(store).summary(head).scale == scale,
                "Restart lost preferences, collector, card identity or style");
    }
    {
        Desktop host(exe, path, true, L"delete");
        const auto card = host.floating_window();
        const auto close_point = floating_close_point(card);
        click(card, close_point);
        require(host.contains("delete " + floating + " failed") && (GetWindowLongPtrW(card, GWL_STYLE) & WS_VISIBLE),
                "Rejected direct deletion closed the window");
        const auto body = at_dip(card, 100, 80);
        pointer(card, WM_LBUTTONDBLCLK, close_point, MK_LBUTTON);
        pointer(card, WM_LBUTTONUP, body);
        require(IsWindow(card) && !host.contains("delete " + floating + " committed"),
                "Floating close followed the pointer onto the body");
        pointer(card, WM_LBUTTONDBLCLK, close_point, MK_LBUTTON);
        SendMessageW(card, WM_CANCELMODE, 0, 0);
        pointer(card, WM_LBUTTONUP, close_point);
        require(IsWindow(card) && !host.contains("delete " + floating + " committed"),
                "Cancelled Floating close consumed release");
        click(card, floating_close_point(card), WM_LBUTTONDBLCLK);
        require(until([&] { return !IsWindow(card); }) && host.contains("delete " + floating + " committed"),
                "Retry did not delete the Floating card");
        SendMessageW(host.workspace, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::rebin), 0);
        close_hanging(host, head);
        require(order(path, domain::rebin_id) == Order{next,last} && order(path, "main") == Order{shared},
                "Deleting inside ReBin recycled again or damaged the shared copy");
        close_host(host);
    }
    const auto original = path / "original.txt";
    std::ofstream(original) << "Synthetic source file";
    domain::Id reference, detached;
    {
        HistoryStore reopened(path);
        history_fixture::rejects([&] { reopened.entry(head); }, "Deleted ReBin entry reappeared");
        history_fixture::rejects([&] { reopened.entry(floating); }, "Deleted Floating entry reappeared");
        require(reopened.entry(shared).snapshot_id == snapshot &&
                reopened.load(shared).snapshot.formats.front().bytes == sample::formats(false).front().bytes,
                "Deletion lost shared content");
        reset_global_preferences(reopened.database());
        require(read_card_preferences(reopened.database()) == application::CardPreferences{}, "Global reset lost deletion defaults");
        domain::Format file;
        file.standard_id = CF_HDROP;
        file.bytes = encode_file_group({original.wstring()});
        reference = reopened.admit(ordinary, {{file}});
        detached = reopened.copy(shared, "main");
        reopened.detach(detached, {"", 40, 710, 240, 160});
    }
    Desktop defaults(exe, path, true);
    close_hanging(defaults, reference);
    const auto card = defaults.floating_window();
    click(card, floating_close_point(card));
    close_host(defaults);
    HistoryStore reopened(path);
    history_fixture::rejects([&] { reopened.entry(reference); }, "Default Hanging deletion retained the entry");
    require(fs::is_regular_file(original) && order(path, domain::rebin_id) == Order{detached,next,last} &&
            reopened.entry(detached).snapshot_id == snapshot, "Default close lost the original file or recycled card");
}

HWND delete_preview(Desktop& host, const domain::Id& id) {
    DWORD pid{};
    GetWindowThreadProcessId(host.workspace, &pid);
    const auto point = center(row(host, id));
    require(PostMessageW(host.workspace, WM_CONTEXTMENU, reinterpret_cast<WPARAM>(host.workspace),
                         MAKELPARAM(point.x, point.y)) != FALSE, "Could not open the row menu");
    HWND action{};
    DWORD_PTR painted{};
    bool answered{};
    require(until([&] {
        auto menu = owned_window(pid, L"ClipRope.RopeMenu");
        if (!menu || !(action = GetDlgItem(menu, 110)) || !IsWindowEnabled(action)) return false;
        answered = SendMessageTimeoutW(menu, cliprope::win::menu_timing_query,
            static_cast<WPARAM>(cliprope::win::MenuStage::painted), 0, SMTO_ABORTIFHUNG, 200, &painted) != 0;
        return answered && painted;
    }, 8s), "Delete action did not finish opening on the private station");
    require(PostMessageW(action, BM_CLICK, 0, 0) != FALSE, "Could not invoke Delete");
    HWND confirm{};
    require(until([&] {
        auto dialog = owned_window(pid, L"ClipRope.RopeManager");
        return dialog && (confirm = GetDlgItem(dialog, IDOK)) && IsWindowEnabled(confirm);
    }), "Delete confirmation button unavailable");
    SendMessageW(confirm, BM_CLICK, 0, 0);
    return confirm;
}

void capture_races(const fs::path& exe, const fs::path& path, const wchar_t* source) {
    const auto seed = prepare(path);
    {
        Desktop host(exe, path, true, nullptr, false, true);
        Observation db(path);
        overview(host, seed.c);
        click(host.workspace, center(row(host, seed.c)));
        require(db.active() == seed.b, "Short press redirected the next copy");
        {
            probe::ExternalCopy delayed(source, L"delayed");
            require(until([&] { return WaitForSingleObject(delayed.reading, 0) == WAIT_OBJECT_0; }), "Capture did not begin on B");
            select(host, db, seed.c);
            SetEvent(delayed.proceed);
            require(until([&] { return db.ids(seed.b).size() == 2; }), "Switched capture lost original target");
            require(db.ids(seed.c) == Order{seed.tail_c}, "Switched capture entered C");
            require(until([&] { return host.contains("follow-head published " + seed.tail_c); }),
                    "Collector switch never published after the bound capture completed");
        }
        {
            probe::ExternalCopy next(source, L"delayed");
            SetEvent(next.proceed);
            require(until([&] { return db.ids(seed.c).size() == 2; }), "Next copy did not enter double-clicked rope");
        }
        const auto collected = db.ids(seed.c);
        select(host, db, seed.b);
        auto confirm = delete_preview(host, seed.b);
        {
            probe::ExternalCopy delayed(source, L"delayed");
            require(until([&] { return WaitForSingleObject(delayed.reading, 0) == WAIT_OBJECT_0; }), "Second capture did not begin");
            SendMessageW(confirm, BM_CLICK, 0, 0);
            require(until([&] { return !db.exists(seed.b) && db.active() == "main"; }), "Bound rope deletion did not commit");
            SetEvent(delayed.proceed);
            require(until([&] { return host.contains("capture history-failed"); }), "Deleted-target capture missing rejection");
            require(db.ids(seed.c) == collected && db.ids("main").empty(), "Deleted-target copy admitted elsewhere");
        }
        require(host.floating_window() != nullptr, "Deleting original rope removed the separate Floating window");
        close_host(host);
    }
    {
        Desktop restarted(exe, path);
        require(restarted.contains("floating " + seed.floating) && restarted.floating_window() != nullptr,
                "Restart failed to restore independent Floating entry");
        close_host(restarted);
    }
    const auto report = history_fixture::diagnostic_report(path);
    require(report.find("Capture history: stale=1 busy=0 storage=0\r\n") != report.npos,
            "Deleted capture destination was not identified exactly once");
    HistoryStore reopened(path);
    require(reopened.floating().size() == 1 && reopened.floating().front().id == seed.floating, "Floating query lost identity");
    std::cout << "B=" << seed.b << " C=" << seed.c << " late-copy=only-B; deleted-B=zero admissions; "
                 "restart-floating=" << seed.floating << '\n';
}

void return_race(const fs::path& exe, const fs::path& path, std::string_view action) {
    const auto seed = prepare(path);
    Desktop host(exe, path, true, nullptr, true);
    Observation db(path);
    overview(host, seed.c);
    HWND floating = host.floating_window();
    require(floating != nullptr, "Floating control unavailable");
    const auto before = GetClipboardSequenceNumber();
    const auto body = at_dip(floating, 115, 80);
    if (action == "drop") {
        pointer(floating, WM_LBUTTONDOWN, body, MK_LBUTTON);
        auto dest = center(row(host, seed.c));
        pointer(floating, WM_MOUSEMOVE, dest, MK_LBUTTON);
        pointer(floating, WM_LBUTTONUP, dest);
        require(until([&] { return db.ids(seed.c) == Order{seed.tail_c, seed.floating}; }), "Explicit tail drop ignored C");
        require(db.active() == seed.b && db.ids(seed.b) == Order{seed.tail_b}, "Drop switched collector or changed B");
    } else {
        click(floating, at_dip(floating, 40, 18));
        require(host.contains("pending-floating " + seed.floating), "Gesture did not bind before collector switch");
        require(db.ids(seed.b) == Order{seed.tail_b} && IsWindow(floating), "Return committed before test barrier");
        select(host, db, seed.c);
        require(until([&] { return db.ids(seed.b) == Order{seed.floating, seed.tail_b}; }), "Pending return did not target B");
        require(db.ids(seed.c) == Order{seed.tail_c} && db.active() == seed.c, "Pending action changed new collector");
    }
    require(until([&] { return !IsWindow(floating); }), "Committed return left a Floating HWND");
    if (action == "drop") require(GetClipboardSequenceNumber() == before, "Drop on inactive rope published");
    else require(host.contains("follow-head published " + seed.tail_c) &&
                 !host.contains("follow-head published " + seed.floating) && clipboard_text_is(sample::text(false)),
                 "Deferred return overwrote the newly selected collector's head");
    close_host(host);
    if (action != "drop") require(probe::clear_private_sample(sample::text(false)), "Could not retire the switched head");
    std::cout << action << " entry=" << seed.floating << " target=" << (action == "drop" ? seed.c : seed.b)
              << " collector=" << db.active() << '\n';
}

void stale_drag(const fs::path& path) {
    const auto seed = prepare(path);
    HistoryStore store(path);
    const auto press = store.entry(seed.tail_b);
    store.move(press.id, "main", &press);
    history_fixture::rejects([&] { store.move(press.id, seed.c, &press); }, "Stale move accepted into C");
    history_fixture::rejects([&] { store.copy(press.id, seed.c, &press); }, "Stale copy accepted into C");
    require(store.entry(press.id).rope_id == "main" && store.hanging(seed.c).size() == 1 &&
            store.hanging(seed.c).front().id == seed.tail_c, "Rejected transfer changed membership or target order");
    std::cout << "stale-move=rolled-back stale-copy=rolled-back\n";
}

void submitted_head(const fs::path& exe, const fs::path& path, const wchar_t* source, bool reject) {
    domain::Entry original;
    {
        HistoryStore seed(path);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = sample::formats(false).front().bytes;
        original = seed.entry(seed.admit("main", {{text}}));
    }
    const auto& id = original.id;
    probe::ExternalCopy sentinel(source, L"second");
    Desktop host(exe, path, true);
    const auto before = GetClipboardSequenceNumber();
    const auto owner = GetClipboardOwner();
    const auto point = center(hanging_bounds(host, id));
    DWORD pid{};
    GetWindowThreadProcessId(host.workspace, &pid);
    require(PostMessageW(host.workspace, WM_CONTEXTMENU, WPARAM(host.workspace), MAKELPARAM(point.x, point.y)),
            "Could not open the head card menu");
    HWND edit{};
    require(until([&] {
        const auto menu = owned_window(pid, L"ClipRope.CardMenu");
        edit = GetDlgItem(menu, int(win::CardCommand::edit));
        return edit && IsWindowEnabled(edit);
    }), "Edit action unavailable");
    require(PostMessageW(edit, BM_CLICK, 0, 0), "Could not select Edit");
    HWND editor{}, field{};
    require(until([&] {
        editor = owned_window(pid, L"ClipRope.InlineEditorHost");
        field = GetDlgItem(editor, 1760);
        return field && GetWindow(editor, GW_OWNER) == host.workspace;
    }), "Head editor unavailable");
    const wchar_t draft[] = L"Submitted head";
    require(SendMessageW(field, WM_SETTEXT, 0, LPARAM(draft)), "Could not enter draft");
    {
        auto observer = history_fixture::observe(path, SQLITE_OPEN_READWRITE);
        Transaction lock(observer.get());
        if (reject)
            exec(observer.get(), "CREATE TRIGGER reject_edit BEFORE UPDATE OF snapshot_id ON entries "
                 "BEGIN SELECT RAISE(ABORT,'fixture edit failure'); END");
        SendMessageW(editor, WM_COMMAND, IDOK, 0);
        require(!(GetWindowLongPtrW(editor, GWL_STYLE) & WS_VISIBLE), "Edit was not submitted");
        SendMessageW(host.workspace, WM_COMMAND, WPARAM(win::WorkspaceCommand::head), 0);
        Statement head(observer.get(), "SELECT snapshot_id FROM entries WHERE id=?");
        head.bind(1, id);
        require(head.step() && head.text(0) == original.snapshot_id && GetClipboardSequenceNumber() == before &&
                GetClipboardOwner() == owner && !host.contains("invoke head"), "Head was published before edit commit");
        lock.commit();
    }
    const auto invoked = "invoke head " + id + " published skipped committed";
    if (reject) {
        require(until([&] { return GetWindowLongPtrW(editor, GWL_STYLE) & WS_VISIBLE; }), "Rejected draft did not reopen");
        wchar_t restored[64]{};
        SendMessageW(field, WM_GETTEXT, 64, LPARAM(restored));
        require(std::wstring_view(restored) == draft && GetClipboardOwner() == owner && GetClipboardSequenceNumber() == before &&
                clipboard_text_is(sample::text(true)) && !host.contains("invoke head") && !host.contains("follow-head published"),
                "Rejected edit lost the draft or replaced the external clipboard");
        SendMessageW(editor, WM_COMMAND, IDCANCEL, 0);
    } else {
        require(until([&] { return host.contains(invoked) && host.contains("follow-head published " + id); }) &&
                GetClipboardSequenceNumber() != before && GetClipboardOwner() != owner && clipboard_text_is(draft),
                "Manual head invocation did not publish the edited snapshot after commit");
        // The receiver explicitly requests a manual paste. The head command skips native dispatch.
        HWND receiver = CreateWindowW(L"EDIT", L"", WS_POPUP, 0, 0, 240, 60, nullptr, nullptr, nullptr, nullptr);
        require(receiver != nullptr, "Manual paste receiver unavailable");
        SendMessageW(receiver, WM_PASTE, 0, 0);
        wchar_t received[64]{};
        GetWindowTextW(receiver, received, 64);
        DestroyWindow(receiver);
        require(std::wstring_view(received) == draft, "Native Edit received the wrong publication");
    }
    require(GetWindowLongPtrW(host.workspace, GWL_STYLE) & WS_VISIBLE, "Head command collapsed the workspace");
    const auto after = GetClipboardSequenceNumber();
    close_host(host);
    std::ifstream receipt(path / "desktop-test.txt");
    std::string line;
    int followed{}, recalled{};
    while (std::getline(receipt, line)) {
        followed += line == "follow-head published " + id;
        recalled += line == invoked;
    }
    require(receipt.eof() && followed == !reject && recalled == !reject, "Wrong head publication or invocation count");
    if (!reject) require(probe::clear_private_sample(draft), "Could not retire the private edited head");
    HistoryStore reopened(path);
    const auto saved = reopened.entry(id);
    require(order(path) == Order{id} && saved.revision == original.revision + (reject ? 0 : 1) &&
            (saved.snapshot_id == original.snapshot_id) == reject &&
            reopened.begin_text_edit(id).text == (reject ? sample::text(false) : draft), "Restart lost committed head state");
    std::cout << "head-edit reject=" << reject << " id=" << id << " original=" << original.snapshot_id
              << " reopened=" << saved.snapshot_id << " sequence=" << before << "->" << after
              << " native-dispatch=0 manual-pastes=" << (reject ? 0 : 1) << '\n';
}

void edited_return(const fs::path& exe, const fs::path& profile, bool reject_first) {
    domain::Entry original;
    domain::Id tail;
    {
        HistoryStore seed(profile);
        seed.set_capacity("main", 1);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = sample::formats(false).front().bytes;
        domain::Snapshot value{{text}};
        const auto id = seed.admit("main", value);
        seed.detach(id, {"", 40, 300, 240, 160});
        application::CardStyle layered;
        layered.opacity = .9;
        seed.save_style(id, layered, false);
        original = seed.entry(id);
        tail = seed.admit("main", value);
    }
    Desktop host(exe, profile, true);
    Observation db(profile);
    const HWND card = host.floating_window();
    require(card != nullptr, "Edited return did not restore the Floating window");
    DWORD pid{};
    GetWindowThreadProcessId(card, &pid);
    const auto body = at_dip(card, 95, 48);
    click(card, body);
    click(card, body, WM_LBUTTONDBLCLK);
    HWND editor{}, field{};
    require(until([&] {
        editor = owned_window(pid, L"ClipRope.InlineEditorHost");
        field = GetDlgItem(editor, 1760);
        return field && GetWindow(editor, GW_OWNER) == card;
    }), "Floating body double-click did not open its editor");
    constexpr wchar_t draft[] = L"Saved before returning";
    require(SendMessageW(field, WM_SETTEXT, 0, LPARAM(draft)), "Could not enter Floating draft");
    const auto sequence = GetClipboardSequenceNumber();
    const auto receipt = "return " + original.id + " committed";
    auto writer = history_fixture::observe(profile, SQLITE_OPEN_READWRITE);
    {
        Transaction lock(writer.get());
        if (reject_first)
            exec(writer.get(), "CREATE TRIGGER reject_edit BEFORE UPDATE OF snapshot_id ON entries "
                               "BEGIN SELECT RAISE(ABORT,'fixture edit failure'); END");
        const auto button = at_dip(card, 20, 18);
        pointer(card, WM_LBUTTONDOWN, button, MK_LBUTTON);
        GUITHREADINFO input{sizeof(input)};
        require(GetGUIThreadInfo(GetWindowThreadProcessId(card, nullptr), &input) && input.hwndCapture == card,
                "Return control did not capture the press while editing");
        pointer(card, WM_LBUTTONUP, button);
        require(db.ids("main") == Order{tail} && IsWindow(card) && !host.contains(receipt) &&
                GetClipboardSequenceNumber() == sequence, "Return preceded the held edit commit");
        lock.commit();
    }
    if (reject_first) {
        require(until([&] {
            wchar_t message[256]{};
            return (GetWindowLongPtrW(editor, GWL_STYLE) & WS_VISIBLE) &&
                   SendMessageW(GetDlgItem(editor, 1761), WM_GETTEXT, 256, LPARAM(message)) &&
                   std::wstring_view(message).find(L"failed") != std::wstring_view::npos;
        }), "Rejected edit did not restore the editor and its error");
        wchar_t restored[64]{};
        SendMessageW(field, WM_GETTEXT, 64, LPARAM(restored));
        require(std::wstring_view(restored) == draft && IsWindow(card) && db.ids("main") == Order{tail} &&
                GetClipboardSequenceNumber() == sequence && !host.contains(receipt), "Rejected save changed the return target");
        exec(writer.get(), "DROP TRIGGER reject_edit");
        click(card, at_dip(card, 20, 18));
    }
    if (!until([&] { return !IsWindow(card) && host.contains(receipt) && clipboard_text_is(draft); })) {
        wchar_t notice[256]{};
        SendMessageW(GetDlgItem(editor, 1761), WM_GETTEXT, 256, LPARAM(notice));
        persistence::Statement state(writer.get(), "SELECT state, revision, snapshot_id FROM entries WHERE id=?");
        state.bind(1, original.id);
        if (state.step()) std::cerr << "entry=" << state.text(0) << " revision=" << state.integer(1)
                                   << " original-snapshot=" << (state.text(2) == original.snapshot_id) << '\n';
        std::wcerr << L"HWND alive=" << IsWindow(card) << L" editor shown="
                   << bool(GetWindowLongPtrW(editor, GWL_STYLE) & WS_VISIBLE) << L" read-only="
                   << bool(GetWindowLongPtrW(field, GWL_STYLE) & ES_READONLY) << L" notice=" << notice << L'\n';
        throw std::runtime_error("Saved return did not retire its window and publish the edited head");
    }
    close_host(host);
    std::ifstream receipts(profile / "desktop-test.txt");
    std::string line;
    unsigned returns{};
    while (std::getline(receipts, line)) returns += line == receipt;
    require(receipts.eof() && returns == 1, "Edited return did not complete exactly once");
    require(probe::clear_private_sample(draft), "Could not retire the edited return sample");
    HistoryStore reopened(profile);
    require(order(profile) == Order{original.id} && reopened.floating().empty() &&
            reopened.begin_text_edit(original.id).text == draft && reopened.entry(original.id).snapshot_id != original.snapshot_id,
            "Reopen lost the edited card, its order or its membership");
}

void scroll_anchor(const fs::path& exe, const fs::path& profile, const wchar_t* source) {
    {
        HistoryStore store(profile);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = sample::formats(false).front().bytes;
        for (unsigned i = 0; i < 32; ++i) store.admit("main", {{text}});
    }
    probe::ExternalCopy sentinel(source, L"second");
    Desktop host(exe, profile, true);
    Observation db(profile);
    const auto before = db.ids("main");
    const auto seq = GetClipboardSequenceNumber();
    const HWND owner = GetClipboardOwner();
    const auto point = center(hanging_bounds(host, before.front()));
    SendMessageW(host.workspace, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), MAKELPARAM(point.x, point.y));
    auto reader = history_fixture::observe(profile, SQLITE_OPEN_READONLY);
    require(until([&] {
        Statement row(reader.get(), "SELECT anchor_entry_id FROM rope_view_state WHERE rope_id='main'");
        return row.step() && row.text(0) == before[1];
    }), "Wheel did not persist the second card as anchor");
    require(db.ids("main") == before && GetClipboardOwner() == owner && GetClipboardSequenceNumber() == seq &&
            clipboard_text_is(sample::text(true)) && !host.contains("follow-head published"),
            "Saving the scroll anchor reordered history or replaced the clipboard");
    close_host(host);
    HistoryStore reopened(profile);
    require(Browser(reopened).load_anchor("main").entry == before[1] && order(profile) == before,
            "Restart lost the saved anchor or changed rope order");
}

void missing_head(const fs::path& exe, const fs::path& profile, const wchar_t* source) {
    domain::Id other, entry;
    {
        HistoryStore store(profile);
        other = store.create_rope("Missing file", 3);
        domain::Format missing;
        missing.standard_id = CF_HDROP;
        missing.bytes = encode_file_group({(profile / "unavailable.txt").wstring()});
        entry = store.admit(other, {{missing}});
    }
    probe::ExternalCopy sentinel(source, L"second");
    Desktop host(exe, profile, true);
    Observation db(profile);
    const auto sequence = GetClipboardSequenceNumber();
    overview(host, other);
    select(host, db, other);
    require(host.contains("follow-head failed") && db.ids(other) == Order{entry} &&
            GetClipboardSequenceNumber() == sequence && clipboard_text_is(sample::text(true)),
            "Failed publication lost the collector, file entry or external clipboard");
    close_host(host);
}
}

#include "../performance/middle_desktop.inc"
#include "hidden_restore.inc"

int wmain(int argc, wchar_t** argv) {
    const bool head_edit = argc == 4 && std::wstring_view(argv[3]) == L"--submitted-head";
    const bool scroll = argc == 4 && std::wstring_view(argv[3]) == L"--scroll-anchor";
    const bool middle = argc == 4 && std::wstring_view(argv[2]) == L"--middle-order";
    if ((!head_edit && !scroll && !middle && argc != 3) || !probe::private_station() || FAILED(OleInitialize(nullptr))) return 2;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int result{};
    try {
        history_fixture::Directory test;
        if (std::wstring_view(argv[2]) == L"--hidden-restore") {
            hidden_restore(argv[1], test.path / "window", true);
            hidden_restore(argv[1], test.path / "write", false);
            hidden_peer(argv[1], test.path / "peers");
        } else if (middle) {
            middle_desktop(argv[1], test.path, argv[3]);
        } else if (scroll) {
            scroll_anchor(argv[1], test.path / "scroll", argv[2]);
        } else if (head_edit) {
            submitted_head(argv[1], test.path / "success", argv[2], false);
            submitted_head(argv[1], test.path / "reject", argv[2], true);
        } else if (std::wstring_view(argv[2]) == L"--edited-return") {
            edited_return(argv[1], test.path / "success", false);
            edited_return(argv[1], test.path / "retry", true);
        } else if (std::wstring_view(argv[2]) == L"--observer") {
            for (bool returning : {false, true}) {
                auto action = test.path / (returning ? "return" : "detach");
                for (bool rollback : {false, true})
                    observer_window(argv[1], action / (rollback ? "rollback" : "notify"), returning, rollback);
            }
        } else if (std::wstring_view(argv[2]) == L"--card-delete") {
            rapid_hanging_deletion(argv[1], test.path / "rapid-direct", false);
            rapid_hanging_deletion(argv[1], test.path / "rapid-recycle", true);
            card_deletion(argv[1], test.path / "card-delete");
            std::cout << "Card deletion settings, rollback, clipboard and restart passed\n";
        } else {
            stale_drag(test.path / "stale-drag");
            missing_head(argv[1], test.path / "missing-file", argv[2]);
            for (auto route : {"button", "drop"})
                return_race(argv[1], test.path / route, route);
            capture_races(argv[1], test.path / L"capture", argv[2]);
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    if (std::wstring_view(argv[2]) == L"--card-delete" && clipboard_text_is(sample::text(false)) &&
        !probe::clear_private_sample(sample::text(false))) result = 1;
    OleUninitialize();
    return result;
}
