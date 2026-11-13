#include "desktop_app.hpp"
#include "platform_win/performance_counters.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "persistence/hotkey_settings.hpp"
#include "platform_win/row_input.hpp"
#include "platform_win/card_editor.hpp"
#include <commctrl.h>
#include <atomic>

namespace {
using namespace workspace_fixture;

struct Receiver {
    HWND window{};
    int downs{}, ups{}, characters{};
    RECT work{};
    static LRESULT CALLBACK proc(HWND window, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR value) {
        auto& self = *reinterpret_cast<Receiver*>(value);
        if (w == VK_TAB) {
            if (msg == WM_KEYDOWN) { ++self.downs; return 0; }
            if (msg == WM_KEYUP) { ++self.ups; return 0; }
            if (msg == WM_CHAR) { ++self.characters; return 0; }
        }
        return DefSubclassProc(window, msg, w, l);
    }
    Receiver() {
        require(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0), "Work area unavailable");
        window = CreateWindowExW(WS_EX_TOOLWINDOW, L"EDIT", L"Synthetic selection remains here",
            WS_POPUP | WS_VISIBLE | ES_MULTILINE, work.left, work.top,
            work.right - work.left, work.bottom - work.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window && SetWindowSubclass(window, proc, 1, reinterpret_cast<DWORD_PTR>(this)), "Receiver unavailable");
        focus();
    }
    ~Receiver() { if (window) DestroyWindow(window); }
    void focus() {
        SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        POINT point{work.left + 20, work.bottom - 35};
        aim(window, point);
        mouse(MOUSEEVENTF_LEFTDOWN); mouse(MOUSEEVENTF_LEFTUP);
        require(until([&] { return GetForegroundWindow() == window && GetFocus() == window; }), "Receiver did not take focus");
        SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        SendMessageW(window, EM_SETSEL, 4, 13);
    }
    void intact() const {
        require(GetForegroundWindow() == window && GetFocus() == window, "Hover shortcut stole editor focus");
        DWORD first{}, last{};
        SendMessageW(window, EM_GETSEL, reinterpret_cast<WPARAM>(&first), reinterpret_cast<LPARAM>(&last));
        wchar_t value[80]{};
        GetWindowTextW(window, value, 80);
        require(first == 4 && last == 13 && std::wstring_view(value) == L"Synthetic selection remains here",
                "Hover shortcut altered editor text or selection");
    }
};

class Observe {
public:
    explicit Observe(const fs::path& root) {
        const auto path = (root / "history.db").u8string();
        require(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &db_, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
                "Fixture database unavailable");
        sqlite3_busy_timeout(db_, 1000);
    }
    ~Observe() { sqlite3_close(db_); }
    std::string active() {
        Statement query(db_, "SELECT active_rope_id FROM app_settings");
        require(query.step(), "Collector missing");
        return query.text(0);
    }
    void fail_selection(bool enabled) {
        const char* sql = enabled ? "CREATE TRIGGER shortcut_failure BEFORE UPDATE OF active_rope_id ON app_settings "
            "BEGIN SELECT RAISE(ABORT,'Synthetic collector failure'); END" : "DROP TRIGGER shortcut_failure";
        require(sqlite3_exec(db_, sql, nullptr, nullptr, nullptr) == SQLITE_OK, "Could not configure fixture write failure");
    }
private:
    sqlite3* db_{};
};

void tab(bool ctrl = false, bool shift = false) {
    if (ctrl) key(VK_LCONTROL, true);
    if (shift) key(VK_LSHIFT, true);
    key(VK_TAB, true); key(VK_TAB, false);
    if (shift) key(VK_LSHIFT, false);
    if (ctrl) key(VK_LCONTROL, false);
    brief(90ms);
}
void toggle() {
    key(VK_LCONTROL, true); key(VK_LMENU, true); key(VK_F11, true);
    key(VK_F11, false); key(VK_LMENU, false); key(VK_LCONTROL, false);
    brief(100ms);
}
DWORD process(HWND window) {
    DWORD pid{};
    GetWindowThreadProcessId(window, &pid);
    return pid;
}
HWND focus_of(HWND window) {
    GUITHREADINFO info{sizeof(info)};
    require(GetGUIThreadInfo(GetWindowThreadProcessId(window, nullptr), &info), "Focus query failed");
    return info.hwndFocus;
}
HWND popup(Desktop& app, POINT point) {
    aim(app.workspace, point);
    mouse(MOUSEEVENTF_RIGHTDOWN); mouse(MOUSEEVENTF_RIGHTUP);
    HWND menu{};
    require(until([&] {
        menu = owned_window(process(app.workspace), L"ClipRope.RopeMenu");
        if(!menu) menu=owned_window(process(app.workspace),L"ClipRope.CardMenu");
        return menu && IsWindowVisible(menu);
    }), "Context menu unavailable");
    return menu;
}
void choose(HWND popup_window, UINT command) {
    RECT bounds{};
    auto item=GetDlgItem(popup_window,command);
    require(item && GetWindowRect(item,&bounds),"Menu command missing");
    aim(item,center(bounds));
    mouse(MOUSEEVENTF_LEFTDOWN); mouse(MOUSEEVENTF_LEFTUP);
    require(until([&] { return !IsWindow(popup_window) || !IsWindowVisible(popup_window); }), "Menu did not close");
}
HWND dialog(Desktop& app, const wchar_t* type) {
    HWND result{};
    require(until([&] {
        result = owned_window(process(app.workspace), type);
        return result && IsWindowVisible(result);
    }), "Expected dialog unavailable");
    return result;
}
domain::Snapshot sample_text(char value) {
    domain::Format format;
    format.standard_id = CF_UNICODETEXT;
    format.bytes = {static_cast<unsigned char>(value), 0, 0, 0};
    return {{format}};
}

std::atomic<int> keyboard_installs{};
HHOOK WINAPI fail_first_keyboard(int type, HOOKPROC hook, HINSTANCE module, DWORD thread) {
    if (type == WH_KEYBOARD_LL && keyboard_installs.fetch_add(1) == 0) {
        SetLastError(ERROR_ACCESS_DENIED);
        return nullptr;
    }
    return SetWindowsHookExW(type, hook, module, thread);
}
void hook_recovery(HWND window) {
    win::RowInput input(fail_first_keyboard);
    require(input.start(window) == ERROR_ACCESS_DENIED, "Keyboard failure was not reported");
    require(input.accepts(0), "Fixture did not establish an eligible command context");
    input.suspend();
    require(!input.accepts(0), "Suspension accepted a queued shortcut");
    require(input.start(window) == ERROR_SUCCESS && keyboard_installs == 2, "Keyboard installation did not recover");
    require(!input.accepts(0), "Reopening accepted a stale shortcut");
    input.stop();
}

void run(const fs::path& host) {
    struct InputCleanup {
        POINT original{};
        InputCleanup() { GetCursorPos(&original); }
        ~InputCleanup() {
            INPUT events[6]{};
            const WORD codes[] = {VK_TAB, VK_LCONTROL, VK_LSHIFT, VK_LMENU, VK_ESCAPE, VK_F11};
            for (size_t i = 0; i < std::size(events); ++i) {
                events[i].type = INPUT_KEYBOARD;
                events[i].ki.wVk = codes[i];
                events[i].ki.dwFlags = KEYEVENTF_KEYUP;
            }
            SendInput(static_cast<UINT>(std::size(events)), events, sizeof(INPUT));
            INPUT release{};
            release.type = INPUT_MOUSE;
            release.mi.dwFlags = MOUSEEVENTF_LEFTUP;
            SendInput(1, &release, sizeof(INPUT));
            SetCursorPos(original.x, original.y);
        }
    } cleanup;
    history_fixture::Directory profile;
    std::vector<std::string> ropes{"main"}, cards;
    {
        HistoryStore store(profile.path);
        ropes.push_back(store.create_rope("B", 10));
        ropes.push_back(store.create_rope("C", 10));
        for (size_t i = 0; i < ropes.size(); ++i) cards.push_back(store.admit(ropes[i], sample_text(char('A' + i))));
        const auto floating = store.admit("main", sample_text('F'));
        store.detach(floating, {"", 800, 450, 200, 160});
        store.set_active_rope("main");
        save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
    }
    Receiver receiver;
    Desktop app(host, profile.path, true);
    Observe observe(profile.path);
    SetForegroundWindow(app.workspace);
    require(until([&] { return GetForegroundWindow() == app.workspace; }), "Hook fixture foreground unavailable");
    hook_recovery(receiver.window);
    receiver.focus();
    brief(300ms);
    const auto sequence = GetClipboardSequenceNumber();
    const auto gap = [&] {
        RECT bounds{};
        GetWindowRect(app.workspace, &bounds);
        const POINT point{bounds.left + 5, bounds.top + MulDiv(85, GetDpiForWindow(app.workspace), 96)};
        aim(receiver.window, point);
        return point;
    };
    const POINT outside{receiver.work.left + 10, receiver.work.bottom - 20};
    auto selected = [&](size_t index) {
        require(until([&] { return observe.active() == ropes[index]; }), "Shortcut selected the wrong collector");
        receiver.intact();
    };
    auto passed = [&](auto send) {
        const auto before = receiver.downs;
        const auto releases = receiver.ups;
        send();
        require(until([&] { return receiver.downs == before + 1 && receiver.ups == releases + 1; }),
                "Out-of-scope Tab was not delivered as a complete press");
    };

    gap(); tab(); selected(1);
    require(receiver.downs == 0 && receiver.ups == 0 && receiver.characters == 0, "Hovered Tab reached the editor");
    key(VK_TAB, true); brief(100ms); selected(2);
    key(VK_TAB, true); key(VK_TAB, true); brief(90ms); selected(2);
    aim(receiver.window, outside);
    key(VK_TAB, false); brief(70ms);
    require(receiver.ups == 0, "Owned release leaked after pointer exit");
    key(VK_TAB, true); brief(60ms);
    gap(); key(VK_TAB, true); key(VK_TAB, false); brief(100ms);
    selected(2);
    require(receiver.downs == 2 && receiver.ups == 1, "Held outside Tab changed owner on pointer entry");
    passed([&] { tab(false, true); }); selected(2);
    passed([&] { tab(true, true); }); selected(2);
    tab(true);
    require(until([&] { return app.frame().rows.size() == 3; }), "Ctrl+Tab did not open overview");
    require(app.frame().rows[0].first == ropes[2] && app.frame().rows[1].first == ropes[0] &&
            app.frame().rows[2].first == ropes[1], "Overview did not rotate into single-rope switching order");
    receiver.intact();
    tab(); selected(0);
    require(app.frame().rows.size() == 3, "Overview Tab collapsed the view");
    require(app.frame().rows.front().first == ropes[0], "Overview Tab left the collector below the first row");
    tab(true);
    require(until([&] { return app.frame().rows.size() == 1; }), "Ctrl+Tab did not return to single view");

    aim(app.workspace, center(app.card(cards[0]))); tab(); selected(1);
    aim(app.workspace, single_rope_point(app)); tab(); selected(2);
    const auto before_scope_tests = receiver.downs;
    gap();
    SetWindowPos(receiver.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    passed([&] { tab(); }); selected(2);
    SetWindowPos(receiver.window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const HWND floating = app.floating_window();
    require(floating != nullptr, "Floating fixture unavailable");
    RECT old_float{}, row{};
    GetWindowRect(floating, &old_float); GetWindowRect(app.workspace, &row);
    SetWindowPos(floating, HWND_TOPMOST, row.left + 600, row.top + 50, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
    RECT placed{};
    GetWindowRect(floating, &placed);
    aim(floating, center(placed)); passed([&] { tab(true); }); selected(2);
    SetWindowPos(floating, nullptr, old_float.left, old_float.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    require(receiver.downs == before_scope_tests + 2, "Excluded window input count mismatch");

    gap(); key(VK_TAB, true); brief(100ms); selected(0);
    toggle(); require(!IsWindowVisible(app.workspace), "Global shortcut did not hide workspace");
    const auto ups = receiver.ups;
    key(VK_TAB, false); brief(70ms);
    require(receiver.ups == ups, "Owned release leaked after hiding");
    passed([&] { tab(); });
    toggle(); require(IsWindowVisible(app.workspace), "Global shortcut did not reopen workspace");
    gap(); tab(); selected(1);

    SendMessageW(app.workspace, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::keyboard), 0);
    require(until([&] { return GetForegroundWindow() == app.workspace && focus_of(app.workspace) == app.workspace; }),
            "Keyboard navigation did not focus workspace");
    SetCursorPos(outside.x, outside.y);
    key(VK_TAB, true); brief(100ms);
    require(observe.active() == ropes[2], "Focused Tab outside row failed");
    key(VK_TAB, true); brief(70ms);
    require(observe.active() == ropes[2], "Focused Tab repeated");
    key(VK_TAB, false); tab(false, true);
    require(observe.active() == ropes[2], "Focused Shift+Tab claimed an unbound shortcut");
    tab(); tab();
    require(observe.active() == ropes[1], "Focused navigation did not wrap");
    receiver.focus(); gap();

    const auto assert_dialog_navigation = [&](HWND window) {
        // The test's foreground receiver grants activation to the dialog opened
        // through its normal command, just as a user opening its menu would.
        SetForegroundWindow(window);
        require(until([&] { return GetForegroundWindow() == window; }), "Dialog did not own foreground");
        const auto before = focus_of(window);
        SetCursorPos(row.left + 5, row.top + 85);
        tab();
        require(focus_of(window) != before && observe.active() == ropes[1], "Dialog Tab was consumed by workspace");
        tab(true);
        require(observe.active() == ropes[1] && app.frame().rows.size() == 1, "Dialog Ctrl+Tab navigated workspace");
    };
    auto listener = owned_window(process(app.workspace), L"ClipRope.Listener");
    PostMessageW(listener, WM_COMMAND, 30, 0);
    auto settings = dialog(app, L"ClipRope.Settings");
    assert_dialog_navigation(settings);
    SendMessageW(settings, WM_CLOSE, 0, 0);
    receiver.focus();
    auto menu = popup(app, single_rope_point(app));
    SetCursorPos(row.left + 5, row.top + 85);
    tab(); require(observe.active() == ropes[1], "Menu Tab navigated workspace");
    key(VK_ESCAPE, true); key(VK_ESCAPE, false);
    require(until([&] { return !IsWindow(menu); }), "Menu failed to cancel");
    menu = popup(app, single_rope_point(app));
    choose(menu, 106);
    auto manager = dialog(app, L"ClipRope.RopeManager");
    assert_dialog_navigation(manager);
    SendMessageW(manager, WM_CLOSE, 0, 0);
    menu = popup(app, center(app.card(cards[1])));
    choose(menu, static_cast<UINT>(win::CardCommand::edit));
    auto editor = dialog(app, L"ClipRope.InlineEditorHost");
    assert_dialog_navigation(editor);
    receiver.focus();
    gap();
    passed([&] { tab(true); });
    require(observe.active() == ropes[1] && app.frame().rows.size() == 1,
            "Unfocused inline draft allowed hovered rope navigation");
    SendMessageW(editor, WM_COMMAND, IDCANCEL, 0);
    receiver.focus();

    aim(app.workspace, single_rope_point(app));
    mouse(MOUSEEVENTF_LEFTDOWN); brief(60ms);
    passed([&] { tab(); });
    require(observe.active() == ropes[1], "Rope grip accepted navigation");
    mouse(MOUSEEVENTF_LEFTUP); brief(70ms);

    // Returning a Floating preview to the desktop must re-enable hover keys,
    // including when leaving the row already cleared its insertion marker.
    RECT before_float{};
    GetWindowRect(floating, &before_float);
    aim(floating, center(before_float));
    mouse(MOUSEEVENTF_LEFTDOWN);
    SetCursorPos(row.left + 500, row.top + 100);
    require(until([&] {
        RECT preview{};
        GetWindowRect(floating, &preview);
        return preview.bottom - preview.top != before_float.bottom - before_float.top;
    }), "Floating drag did not enter the rope preview");
    const auto return_point = center(before_float);
    SetCursorPos(return_point.x, return_point.y);
    require(until([&] {
        RECT preview{};
        GetWindowRect(floating, &preview);
        return preview.bottom - preview.top == before_float.bottom - before_float.top;
    }), "Floating drag did not leave the rope preview");
    mouse(MOUSEEVENTF_LEFTUP); brief(70ms);
    gap(); tab(); selected(2); tab(); selected(0);
    tab(); selected(1);
    gap();
    observe.fail_selection(true);
    tab();
    require(until([&] { return app.contains("command failed"); }) && observe.active() == ropes[1],
            "Failed collector write changed selection or omitted the error");
    observe.fail_selection(false);
    tab(); selected(2);
    require(GetClipboardSequenceNumber() != sequence && clipboard_text_is(L"C"), "Navigation did not copy C's head");
    for (size_t i = 0; i < ropes.size(); ++i)
        require(order(profile.path, ropes[i]) == Order{cards[i]}, "Navigation changed card order");
    app.stop();
    require(app.clean_exit, "Shortcut host did not exit cleanly");
    {
        HistoryStore store(profile.path);
        require(store.active_rope() == ropes[2], "Collector did not survive restart");
    }
    history_fixture::Directory single_profile;
    {
        HistoryStore store(single_profile.path);
        store.admit("main", sample_text('S'));
        save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
    }
    Desktop single(host, single_profile.path, true);
    receiver.focus();
    RECT bounds{};
    GetWindowRect(single.workspace, &bounds);
    aim(receiver.window, {bounds.left + 5, bounds.top + MulDiv(85, GetDpiForWindow(single.workspace), 96)});
    const auto before_single = receiver.downs;
    tab();
    require(receiver.downs == before_single && !single.contains("command failed"), "Single-rope shortcut failed");
    single.stop();
    require(single.clean_exit, "Single-rope host did not exit cleanly");
    std::cout << "hover=line,card,gap focus=preserved overview=toggle repeat=once pairing=preserved "
                 "excluded=outside,occluded,floating,settings,manager,editor,menu,drag "
                 "rollback=passed restart=passed hook_retry=passed single_rope=passed\n";
}
}
namespace {
void find_desktop(const fs::path& host) {
    history_fixture::Directory profile;
    std::string target, other, second;
    {
        HistoryStore store(profile.path);
        // The Find scenario uses scoped Ctrl+F. A system-wide shortcut conflict
        // would otherwise open Settings over the workspace before the first hover.
        const auto defaults = application::default_shortcuts();
        for (auto action : {application::workspace_toggle, application::quick_note})
            save_shortcut(store.database(), action, {false, defaults[action].chord});
        auto sample = [](std::wstring text) {
            domain::Format f;
            f.standard_id = CF_UNICODETEXT;
            f.bytes.resize((text.size() + 1) * 2);
            std::memcpy(f.bytes.data(), text.data(), text.size() * 2);
            return domain::Snapshot{{std::move(f)}};
        };
        store.set_capacity("main", 101);
        target = store.admit("main", sample(std::wstring(3800, L'x') + L"😀磁性 磁性 磁性"));
        domain::Format png;
        png.name = "PNG";
        png.bytes = media_sample::png().bytes;
        for (int i = 1; i < 100; ++i)
            store.admit("main", i >= 40 && i < 60 ? domain::Snapshot{{png}} : sample_text('x'));
        other = store.create_rope("Work", 10);
        second = store.admit(other, sample(L"磁性"));
        store.set_active_rope("main");
    }
    Receiver external;
    Desktop app(host, profile.path, true);
    external.focus();
    POINT cursor{};
    GetCursorPos(&cursor);
    struct Restore {
        POINT p;
        ~Restore() {
            SetCursorPos(p.x, p.y);
        }
    } restore{cursor};
    auto chord = [] {
        key(VK_LCONTROL, true);
        key('F', true);
        key('F', false);
        key(VK_LCONTROL, false);
    };
    const auto sequence = GetClipboardSequenceNumber();
    auto preview_decodes = [&] {
        auto listener = owned_window(process(app.workspace), L"ClipRope.Listener");
        return SendMessageW(listener, win::performance_query, WPARAM(win::PerformanceCounter::preview_decodes), 0);
    };
    const auto decodes = preview_decodes();
    const auto before = order(profile.path);
    RECT bounds{};
    GetWindowRect(app.workspace, &bounds);
    SetCursorPos(bounds.left + 5, bounds.top + MulDiv(85, GetDpiForWindow(app.workspace), 96));
    auto started = std::chrono::steady_clock::now();
    chord();
    auto panel = dialog(app, L"ClipRope.FindInCards");
    auto edit = GetDlgItem(panel, 1);
    if (!until([&] { return GetForegroundWindow() == panel && focus_of(panel) == edit; })) {
        std::cerr << "Foreground is panel=" << (GetForegroundWindow() == panel)
                  << ", field focused=" << (focus_of(panel) == edit) << "\n";
        throw std::runtime_error("Find did not acquire native input");
    }
    auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started)
            .count();
    };
    std::cout << "First open " << elapsed() << " ms\n";
    auto status = [&](std::wstring_view value) {
        require(until([&] {
                    wchar_t count[80]{};
                    SendMessageW(GetDlgItem(panel, 2), WM_GETTEXT, 80, LPARAM(count));
                    return count == value;
                }),
                "Find count or selection did not settle");
    };
    auto search = [&](const wchar_t* value) { SendMessageW(edit, WM_SETTEXT, 0, LPARAM(value)); };
    started = std::chrono::steady_clock::now();
    search(L"absent");
    search(L"x");
    search(L"磁性");
    status(L"1 / 3");
    SendMessageW(edit, WM_IME_STARTCOMPOSITION, 0, 0);
    PostMessageW(edit, WM_KEYDOWN, VK_RETURN, 0);
    brief(40ms);
    status(L"1 / 3");
    SendMessageW(edit, WM_IME_ENDCOMPOSITION, 0, 0);
    app.card(target);
    std::cout << "Rapid query to deep card " << elapsed() << " ms\n";
    key(VK_RETURN, true);
    key(VK_RETURN, false);
    status(L"2 / 3");
    SendMessageW(GetDlgItem(panel, 5), BM_CLICK, 0, 0);
    status(L"3 / 3");
    SendMessageW(GetDlgItem(panel, 5), BM_CLICK, 0, 0);
    status(L"1 / 3");
    external.focus();
    require(IsWindowVisible(panel), "Ordinary blur closed Find");
    SendMessageW(app.workspace, WM_COMMAND, UINT(win::WorkspaceCommand::overview), 0);
    status(L"1 / 4");
    SendMessageW(GetDlgItem(panel, 4), BM_CLICK, 0, 0);
    status(L"4 / 4");
    app.card(second);
    require(Observe(profile.path).active() == "main", "Find selected another collector");
    require(preview_decodes() == decodes, "Finding decoded offscreen images");
    require(order(profile.path) == before && GetClipboardSequenceNumber() == sequence,
            "Find reordered cards or published content");
    SetForegroundWindow(panel);
    SendMessageW(panel, WM_CLOSE, 0, 0);
    require(!IsWindow(panel), "Find failed to close");
    app.card(second);
    auto row = app.frame().rows.front().second;
    auto menu = popup(app, center(row));
    started = std::chrono::steady_clock::now();
    choose(menu, UINT(win::WorkspaceCommand::find));
    panel = dialog(app, L"ClipRope.FindInCards");
    edit = GetDlgItem(panel, 1);
    std::cout << "Repeated menu open " << elapsed() << " ms\n";
    SetWindowPos(panel, nullptr, external.work.left + 20, external.work.bottom - 130, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    search(L"磁性");
    status(L"1 / 4");
    SendMessageW(GetDlgItem(panel, 4), BM_CLICK, 0, 0);
    status(L"4 / 4");
    menu = popup(app, center(app.card(second)));
    choose(menu, UINT(win::CardCommand::edit));
    auto editor = dialog(app, L"ClipRope.InlineEditorHost");
    auto field = GetDlgItem(editor, 1760);
    require(field && SendMessageW(field, WM_SETTEXT, 0, LPARAM(L"磁性 磁性")), "Cannot enter inline draft");
    status(L"4 / 4"); // Draft is private until Save.
    SendMessageW(editor, WM_COMMAND, IDOK, 0);
    status(L"4 / 5");
    const auto rect = app.card(second);
    const auto dpi = GetDpiForWindow(app.workspace);
    aim(app.workspace, {rect.right - MulDiv(13, dpi, 96), rect.top + MulDiv(12, dpi, 96)});
    mouse(MOUSEEVENTF_LEFTDOWN);
    mouse(MOUSEEVENTF_LEFTUP);
    status(L"1 / 3");
    app.card(target);
    require(GetClipboardSequenceNumber() == sequence, "Editing or deleting during Find published content");
    search(L"磁");
    status(L"1 / 3");
    ClipboardPublisher producer;
    const auto captured = external_copy(producer, profile.path, L'磁');
    status(L"2 / 4"); // The surviving match stays selected after a new head arrives.
    require(order(profile.path).front() == captured, "Cross-rope find redirected external capture");
    external.focus();
    SetCursorPos(bounds.left + 5, bounds.top + 85);
    chord();
    require(until([&] { return GetForegroundWindow() == panel; }), "Could not refocus existing query");
    search(L"");
    status(L"");
    key(VK_ESCAPE, true);
    key(VK_ESCAPE, false);
    require(until([&] { return !IsWindow(panel); }), "Escape did not dismiss Find");
    auto listener = owned_window(process(app.workspace), L"ClipRope.Listener");
    // A conflict can already have opened Settings. Finish the tray command before using its rebuilt controls.
    SendMessageW(listener, WM_COMMAND, 30, 0);
    auto settings = dialog(app, L"ClipRope.Settings");
    SendMessageW(GetDlgItem(settings, 1503), BM_CLICK, 0, 0);
    auto page = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
    auto toggle = GetDlgItem(page, 1816);
    require(toggle && IsWindowEnabled(toggle) && SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_CHECKED,
            "Hover Find preference was not enabled by default");
    SendMessageW(toggle, BM_CLICK, 0, 0);
    require(SendMessageW(toggle, BM_GETCHECK, 0, 0) == BST_UNCHECKED, "Preference did not toggle");
    SendMessageW(settings, WM_CLOSE, 0, 0);
    external.focus();
    SetCursorPos(bounds.left + 5, bounds.top + 85);
    chord();
    brief(160ms);
    require(!owned_window(process(app.workspace), L"ClipRope.FindInCards"), "Disabled hover shortcut opened Find");
    if (GetForegroundWindow() != external.window) {
        wchar_t cls[96]{};
        DWORD owner{};
        GetClassNameW(GetForegroundWindow(), cls, 96);
        GetWindowThreadProcessId(GetForegroundWindow(), &owner);
        std::wcerr << L"Unexpected foreground class " << cls << L", ClipRope owner " << (owner == process(app.workspace)) << L'\n';
        throw std::runtime_error("Editor lost focus after disabled hover shortcut");
    }
    app.stop();
    require(app.clean_exit, "Find worker did not stop cleanly");
    {
        HistoryStore store(profile.path);
        require(!read_shortcuts(store.database())[application::find_cards].enabled, "Hover preference did not survive reopen");
        const auto deep = store.entry(target);
        require(deep.revision == 1 && deep.rope_id == "main" && deep.sort_key.has_value(),
                "Finding modified the entry revision or placement");
    }
}
// Exercise the recording control with physical input, including an already registered global chord.
void configurable_shortcuts(const fs::path& host) {
    history_fixture::Directory profile;
    std::string other;
    {
        HistoryStore store(profile.path);
        save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
        save_shortcut(store.database(), application::find_cards, {false, "Ctrl+F"});
        other = store.create_rope("Other", 10);
        store.admit("main", sample_text('A'));
        store.set_active_rope("main");
    }
    Receiver external;
    Desktop app(host, profile.path);
    auto pid = process(app.workspace);
    auto listener = owned_window(pid, L"ClipRope.Listener");
    auto open = [&] {
        SendMessageW(listener, WM_COMMAND, 30, 0);
        auto window = dialog(app, L"ClipRope.Settings");
        SetForegroundWindow(window);
        require(until([&] { return GetForegroundWindow() == window; }), "Settings did not take focus");
        SendMessageW(GetDlgItem(window, 1503), BM_CLICK, 0, 0);
        return window;
    };
    HWND settings = open();
    auto page = [&] { return FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr); };
    auto click = [&](int id) { SendMessageW(GetDlgItem(page(), id), BM_CLICK, 0, 0); };
    auto has_text = [&](int id, const wchar_t* text) {
        wchar_t value[200]{};
        SendMessageW(GetDlgItem(page(), id), WM_GETTEXT, std::size(value), LPARAM(value));
        return std::wstring_view(value).find(text) != std::wstring_view::npos;
    };
    auto tip_contains = [&](const wchar_t* text) {
        HWND tip{};
        while ((tip = FindWindowExW(nullptr, tip, L"ClipRope.Tooltip", nullptr))) {
            if (!IsWindowVisible(tip) || GetWindow(tip, GW_OWNER) != settings) continue;
            wchar_t value[256]{};
            SendMessageW(tip, WM_GETTEXT, std::size(value), LPARAM(value));
            if (std::wstring_view(value).find(text) != std::wstring_view::npos) return true;
        }
        return false;
    };
    auto send = [](std::initializer_list<WORD> keys) {
        for (auto code : keys) key(code, true);
        for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) key(*it, false);
        brief(60ms);
    };
    auto record = [&](int id, std::initializer_list<WORD> keys) {
        click(id);
        require(until([&] { return has_text(id, L", recording"); }), "Recorder did not arm");
        send(keys);
        require(until([&] { return !has_text(id, L", recording"); }), "Recorder did not finish");
    };
    const auto sequence = GetClipboardSequenceNumber();
    const auto original = order(profile.path);
    // Traverse both ways with real dialog input. Scrolled children remain reachable by ID.
    HWND last = GetDlgItem(page(), 1818);
    HWND first = GetDlgItem(page(), 1800);
    auto traverse = [&](HWND target, bool reverse) {
        for (int i = 0; i < 32 && focus_of(settings) != target; ++i) tab(false, reverse);
        RECT view{}, control{};
        GetWindowRect(page(), &view);
        GetWindowRect(target, &control);
        require(focus_of(settings) == target && control.top >= view.top && control.bottom <= view.bottom,
                "Focused shortcut control was clipped");
    };
    RECT bounds{};
    GetWindowRect(settings, &bounds);
    SetWindowPos(settings, nullptr, 0, 0, bounds.right - bounds.left, 360,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    traverse(last, false);
    traverse(first, true);
    require(has_text(1801, L"Ctrl+Alt+F11") && !SendMessageW(GetDlgItem(page(), 1816), BM_GETCHECK,0,0),
            "Legacy global binding or Find switch was lost");
    click(1810);
    require(has_text(1810,L"Not verified") && tip_contains(L"first opens"), "Unstarted hover service reported available");
    for (int id = 1800; id <= 1816; id += 4) {
        auto toggle = GetDlgItem(page(), id);
        auto previous = SendMessageW(toggle,BM_GETCHECK,0,0);
        click(id);
        require(SendMessageW(toggle,BM_GETCHECK,0,0) != previous, "Shortcut switch did not update");
        click(id+2);
        if (id == 1800) require(has_text(id+2, L"Available (off)"), "Disabled global lost its availability result");
        click(id);
        require(SendMessageW(toggle,BM_GETCHECK,0,0) == previous, "Shortcut switch did not restore");
    }
    record(1801,{VK_LCONTROL,VK_LSHIFT,VK_SPACE});
    require(tip_contains(L"Used by Quick note") && has_text(1801,L"Ctrl+Alt+F11") && !app.floating_window(),
            "Recording an existing global chord dispatched it or changed the original key");
    record(1801,{VK_ESCAPE});
    require(has_text(1801,L"Ctrl+Alt+F11"), "Escape committed a binding");
    require(RegisterHotKey(external.window, 12, MOD_CONTROL | MOD_ALT, VK_F8), "External fixture chord unavailable");
    record(1801,{VK_LCONTROL,VK_LMENU,VK_F8});
    require(tip_contains(L"Unavailable") && has_text(1801,L"Ctrl+Alt+F11"), "External conflict replaced the old key");
    UnregisterHotKey(external.window, 12);
    {
        const auto path = (profile.path / "history.db").u8string();
        sqlite3* db{};
        require(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &db, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
                "Could not open synthetic failure store");
        exec(db, "CREATE TRIGGER reject_chord BEFORE INSERT ON shortcut_preferences BEGIN SELECT RAISE(ABORT,'test'); END");
        record(1801,{VK_LCONTROL,VK_LMENU,VK_F8});
        exec(db, "DROP TRIGGER reject_chord");
        sqlite3_close(db);
        require(tip_contains(L"Could not save") && has_text(1801,L"Ctrl+Alt+F11"), "Failed save changed the binding");
        require(RegisterHotKey(external.window,12,MOD_CONTROL | MOD_ALT,VK_F8), "Failed save leaked registration");
        UnregisterHotKey(external.window,12);
    }
    send({VK_LCONTROL,VK_LMENU,VK_F11});
    require(IsWindowVisible(app.workspace), "Original global binding stopped working after failure");
    settings = open();
    click(1810);
    require(has_text(1810, L"✓ Available"), "Live hover and focused scope did not report available");
    record(1801,{VK_LCONTROL,VK_LMENU,VK_F8});
    require(has_text(1801,L"Ctrl+Alt+F8") && has_text(1802,L"Check availability"), "Global rebind did not commit");
    record(1809,{VK_TAB});
    // The recorder retains focus. Enter arms it again without moving to another control.
    send({VK_RETURN});
    require(has_text(1809,L", recording"), "Enter did not arm the focused recorder");
    send({VK_F6});
    require(has_text(1809,L"F6"), "Keyboard-only recording failed");
    record(1813,{VK_F7});
    record(1805,{VK_LCONTROL,VK_LSHIFT,VK_F9});
    click(1816);
    record(1817,{VK_LCONTROL,'G'});
    SendMessageW(settings,WM_CLOSE,0,0);
    external.focus();
    RECT area{};
    GetWindowRect(app.workspace,&area);
    aim(external.window,{area.left + 5, area.top + MulDiv(85,GetDpiForWindow(app.workspace),96)});
    Observe observe(profile.path);
    send({VK_F7});
    require(until([&] { return app.frame().rows.size() == 2; }), "Custom hover overview shortcut failed");
    send({VK_F6});
    require(until([&] { return observe.active() == other; }), "New hover binding was not active");
    external.intact();
    SendMessageW(app.workspace,WM_COMMAND,WPARAM(win::WorkspaceCommand::keyboard),0);
    SetCursorPos(external.work.left + 10,external.work.bottom - 20);
    send({VK_F6});
    require(until([&] { return observe.active() == "main"; }), "New focused binding was not active");
    send({VK_F7});
    require(until([&] { return app.frame().rows.size() == 1; }), "Custom focused overview shortcut failed");
    send({VK_LCONTROL,'G'});
    auto find = owned_window(pid,L"ClipRope.FindInCards");
    require(find && IsWindowVisible(find), "Focused custom Find shortcut did not open the search field");
    SendMessageW(find, WM_CLOSE, 0, 0);
    settings = open();
    click(1816);
    SendMessageW(settings, WM_CLOSE, 0, 0);
    SendMessageW(app.workspace,WM_COMMAND,WPARAM(win::WorkspaceCommand::keyboard),0);
    send({VK_LCONTROL,'G'});
    require(!owned_window(pid,L"ClipRope.FindInCards"), "Disabled focused Find still opened");
    send({VK_LCONTROL,VK_LMENU,VK_F8});
    require(!IsWindowVisible(app.workspace), "Replacement global binding did not collapse the workspace");
    send({VK_LCONTROL,VK_LSHIFT,VK_F9});
    require(until([&] { return app.floating_window() != nullptr; }), "Custom quick note binding did not create a card");
    require(GetClipboardSequenceNumber() != sequence && clipboard_text_is(L"A") && order(profile.path) == original,
            "Configured shortcuts did not copy the selected main rope's head");
    app.stop();
    require(app.clean_exit, "Shortcuts fixture exit failed");
    HistoryStore reopened(profile.path);
    const auto keys = read_shortcuts(reopened.database());
    require(keys[application::workspace_toggle].chord == "Ctrl+Alt+F8" && keys[application::next_rope].chord == "F6" &&
            keys[application::toggle_overview].chord == "F7" && keys[application::quick_note].chord == "Ctrl+Shift+F9" &&
            keys[application::find_cards] == application::ShortcutBinding{false,"Ctrl+G"}, "Binding did not survive reopen");
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && argc != 3) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr))) return 1;
    int result = 1;
    try {
        const auto host = fs::absolute(argv[1]);
        if (argc == 2) run(host);
        else if (std::wstring_view(argv[2]) == L"--find") find_desktop(host);
        else if (std::wstring_view(argv[2]) == L"--configure") configurable_shortcuts(host);
        else throw std::runtime_error("Unknown shortcut fixture");
        result = 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; }
    OleUninitialize();
    return result;
}
