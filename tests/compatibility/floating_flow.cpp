#include "persistence/hotkey_settings.hpp"
#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"
#include "desktop_app.hpp"

namespace {
using namespace workspace_fixture;

// A test-owned blank area below the workspace receives mouse-up if foreground
// capture is lost on this machine. No click is sent to a user's application.
struct LandingPad {
    HWND window{};
    RECT rect{};
    LandingPad() {
        RECT work{};
        require(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0), "Missing display work area");
        WNDCLASSW surface{};
        surface.hInstance = GetModuleHandleW(nullptr);
        surface.lpfnWndProc = DefWindowProcW;
        surface.lpszClassName = L"ClipRope.TestLandingPad";
        surface.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
        require(RegisterClassW(&surface) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "Drop surface class unavailable");
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, surface.lpszClassName,
            L"ClipRope drag test", WS_POPUP | WS_VISIBLE, work.right - 650,
            work.top + (work.bottom - work.top) / 2, 620, 285, nullptr, nullptr, nullptr, nullptr);
        require(window && GetWindowRect(window, &rect), "Could not create test drop target");
    }
    ~LandingPad() { if (window) DestroyWindow(window); }
    POINT drop_point() const { return {rect.left + 210, rect.top + 60}; }
};

struct Seed {
    std::string x, snapshot, tail;
    Order initial;
};
domain::Snapshot text_sample(bool rich) {
    domain::Snapshot result;
    for (const auto& item : sample::formats(!rich)) {
        if (!rich && item.format != CF_UNICODETEXT) continue;
        domain::Format saved;
        if (item.format == CF_UNICODETEXT) saved.standard_id = item.format;
        else saved.name = "HTML Format";
        saved.bytes = item.bytes;
        result.formats.push_back(std::move(saved));
    }
    return result;
}
Seed seed(const fs::path& profile, int capacity, bool floating) {
    HistoryStore store(profile);
    store.set_capacity("main", capacity);
    save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
    Seed result;
    if (floating) {
        result.x = store.admit("main", text_sample(true));
        // Nonexistent monitor exercises single-display work-area recovery.
        store.detach(result.x, {"Removed test panel", 515, 300, 240, 160});
    }
    for (int i = 0; i < capacity - (floating ? 0 : 1); ++i) store.admit("main", text_sample(false));
    if (!floating) result.x = store.admit("main", text_sample(true));
    result.snapshot = store.entry(result.x).snapshot_id;
    for (const auto& item : store.hanging("main")) result.initial.push_back(item.id);
    result.tail = result.initial.back();
    return result;
}

HWND floating(Desktop& host) {
    HWND window{};
    require(until([&] {
        window = host.floating_window();
        return window && IsWindowVisible(window);
    }), "Independent Floating window unavailable");
    return window;
}
RECT rect_of(HWND hwnd) {
    RECT r{};
    require(GetWindowRect(hwnd, &r), "Test window has no rectangle");
    return r;
}
void drag_to(HWND window, POINT start, POINT destination, bool cancel = false,
             const std::function<void()> &while_held = {}) {
    aim(window, start);
    wchar_t name[64]{};
    GetClassNameW(window, name, 64);
    const bool hanging = std::wstring_view(name) == L"ClipRope.Workspace";
    const auto foreground = GetForegroundWindow();
    mouse(MOUSEEVENTF_LEFTDOWN);
    try {
        HWND preview{};
        RECT initial{};
        if (hanging) {
            require(until([&] {
                const auto input = FindWindowW(L"ClipRope.DragInput", nullptr);
                return input && GetWindow(input, GW_OWNER) == window;
            }), "Press did not capture the pointer");
            start.x += 12;
            require(SetCursorPos(start.x, start.y) != FALSE, "First movement failed");
            require(until([&] {
                preview = FindWindowW(L"ClipRope.DragCard", nullptr);
                return preview && GetWindow(preview, GW_OWNER) == window && IsWindowVisible(preview);
            }), "Holding a Hanging card did not display its independent preview");
            initial = rect_of(preview);
            require(PtInRect(&initial, start), "Lifted card missed the original grip");
        } else {
            brief(80ms);
        }
        require(SetCursorPos(destination.x, destination.y) != FALSE, "Could not drag the synthetic card");
        brief(140ms);
        if (hanging) {
            const auto actual = rect_of(preview);
            const auto tolerance = MulDiv(18, GetDpiForWindow(preview), 96);
            require(PtInRect(&actual, destination) &&
                    std::abs(actual.left - initial.left - (destination.x - start.x)) <= tolerance &&
                    std::abs(actual.top - initial.top - (destination.y - start.y)) <= tolerance,
                    "Preview did not follow the physical pointer outside the workspace before release");
            require(GetForegroundWindow() == foreground, "Hanging drag took focus from the foreground field");
        }
        if (while_held) while_held();
        if (cancel) { key(VK_ESCAPE, true); brief(110ms); key(VK_ESCAPE, false); }
    } catch (...) {
        mouse(MOUSEEVENTF_LEFTUP);
        throw;
    }
    mouse(MOUSEEVENTF_LEFTUP);
    brief(100ms);
}
void button(HWND window, bool erase) {
    RECT client{};
    require(GetClientRect(window, &client) != FALSE, "Floating client bounds unavailable");
    auto dpi = GetDpiForWindow(window);
    POINT p{erase ? client.right - MulDiv(19, dpi, 96) : MulDiv(46, dpi, 96), MulDiv(19, dpi, 96)};
    ClientToScreen(window, &p);
    aim(window, p);
    brief(40ms);
    require(WindowFromPoint(p) == window, "Floating control is covered");
    mouse(MOUSEEVENTF_LEFTDOWN); brief(55ms); mouse(MOUSEEVENTF_LEFTUP);
}
void toggle(Desktop& host, const Fields& fields) {
    require(host.contains("hotkey registered") && GetForegroundWindow() == fields.window,
            "Test shortcut not bound to the fixture");
    key(VK_CONTROL,true); key(VK_MENU,true); key(VK_F11,true);
    key(VK_F11,false); key(VK_MENU,false); key(VK_CONTROL,false);
}
void clean_stop(Desktop& host) {
    host.stop();
    require(host.clean_exit, "Application did not shut down cleanly");
}
void lifecycle(const fs::path& exe, const fs::path& profile, Fields& fields, const LandingPad& pad) {
    auto data = seed(profile, 100, false);
    ClipboardPublisher producer;
    const auto outside = pad.drop_point();
    {
        Desktop host(exe, profile, true, L"detach");
        reset_fields(fields);
        require(WindowFromPoint(outside) == pad.window, "Drop pad unavailable");
        drag_to(host.workspace, center(host.card(data.x)), outside, true);
        require(order(profile) == data.initial && !host.floating_window(), "Canceled detach modified the rope");
        brief(std::chrono::milliseconds(GetDoubleClickTime()+20));
        drag_to(host.workspace, center(host.card(data.x)), outside);
        require(until([&] { return host.contains("detach " + data.x + " failed"); }) &&
                order(profile) == data.initial && !host.floating_window(), "Failed detach did not roll back");
        clean_stop(host);
    }
    RECT moved{};
    std::string replacement;
    {
        Desktop host(exe, profile, true);
        reset_fields(fields);
        require(WindowFromPoint(outside) == pad.window, "Drop destination unavailable");
        const auto sequence = GetClipboardSequenceNumber();
        drag_to(host.workspace, center(host.card(data.x)), outside);
        HWND card = floating(host);
        auto expected = data.initial;
        expected.erase(expected.begin());
        require(order(profile) == expected && GetClipboardSequenceNumber() != sequence &&
                clipboard_text_is(sample::text(true)), "Detach did not copy the succeeding head");
        toggle(host, fields);
        require(until([&] { return !IsWindowVisible(host.workspace) && IsWindowVisible(card); }),
                "Collapsing the workspace hid the Floating card");
        replacement = external_copy(producer, profile, L'D');
        expected.insert(expected.begin(), replacement);
        require(order(profile) == expected, "Refilling the rope evicted an extra entry");
        auto previous = rect_of(card);
        const auto from = center(previous);
        POINT to{from.x + 70, from.y + 42};
        drag_to(card, from, to);
        require(until([&] { return host.contains("move-floating " + data.x + " committed"); }),
                "Floating move was not persisted");
        moved = rect_of(card);
        require(moved.left != previous.left && moved.top != previous.top, "Floating position did not change");
        clean_stop(host);
    }
    {
        HistoryStore store(profile);
        const auto x = store.entry(data.x);
        require(!x.rope_id && !x.sort_key && x.snapshot_id == data.snapshot &&
                store.floating().size() == 1 && store.hanging("main").size() == 100,
                "Detached identity, snapshot or ownership failed to survive exit");
    }
    {
        Desktop host(exe, profile, false, L"return");
        HWND card = floating(host);
        const auto restored = rect_of(card);
        require(std::abs(restored.left-moved.left) <= 1 && std::abs(restored.top-moved.top) <= 1 &&
                !IsWindowVisible(host.workspace), "Restart did not restore the Floating layout independently");
        reset_fields(fields);
        const auto sequence = GetClipboardSequenceNumber();
        button(card, false);
        require(until([&] { return host.contains("return " + data.x + " failed"); }) && IsWindowVisible(card),
                "Failed return hid or deleted the card");
        require(sequence == GetClipboardSequenceNumber(), "Failed return published content");
        brief(std::chrono::milliseconds(GetDoubleClickTime()+20));
        button(card, false);
        require(until([&] { return !IsWindow(card) && order(profile).front() == data.x; }),
                "Return button did not commit and close");
        auto expected = data.initial;
        expected.erase(expected.begin());
        expected.pop_back();
        expected.insert(expected.begin(), replacement);
        expected.insert(expected.begin(), data.x);
        require(order(profile) == expected && !IsWindowVisible(host.workspace) &&
                clipboard_text_is(sample::text(false)) && fields.get(Query::a_unchanged) && !fields.get(Query::changes_a),
                "Return lost the published head, hidden state, original field or expected tail");
        show(host, fields);
        drag_to(host.workspace, center(host.card(data.x)), outside);
        card = floating(host);
        const auto before_drop = order(profile);
        const RECT next = host.card(before_drop.at(2));
        const POINT between{next.left + 8, center(next).y};
        const auto preview_slot = host.card(before_drop.at(3));
        const auto original = rect_of(card);
        const auto preview = [&] {
            const auto carried = rect_of(card);
            require((GetWindowLongPtrW(card, GWL_EXSTYLE) & WS_EX_LAYERED) &&
                    carried.bottom - carried.top == MulDiv(240, GetDpiForWindow(card), 96) &&
                    host.card(before_drop.at(2)).left == preview_slot.left &&
                    order(profile) == before_drop, "Floating drag did not show a provisional hanger and insertion slot");
        };
        drag_to(card, center(original), between, true, preview);
        const auto restored_drag = rect_of(card);
        require(EqualRect(&original, &restored_drag) && host.card(before_drop.at(2)).left == next.left &&
                order(profile) == before_drop, "Cancel did not restore the Floating window and numbered slots");
        brief(std::chrono::milliseconds(GetDoubleClickTime() + 20));
        drag_to(card, center(original), between, false, preview);
        auto inserted = before_drop;
        inserted.insert(inserted.begin() + 2, data.x);
        require(until([&] { return !IsWindow(card) && order(profile) == inserted; }),
                "Dropping Floating between cards did not preserve the previewed order");
        drag_to(host.workspace, center(host.card(data.x)), outside);
        floating(host);
        clean_stop(host);
    }
    {
        domain::Id collector;
        application::CardStyle style;
        style.bold = true;
        style.text_color = 0x123456;
        {
            HistoryStore store(profile);
            collector = store.create_rope("B", 3);
            store.set_active_rope(collector);
            store.save_style(data.x, style);
        }
        Desktop host(exe, profile, false, L"recycle");
        const auto sequence = GetClipboardSequenceNumber();
        HWND card = floating(host);
        reset_fields(fields);
        const auto before_close = rect_of(card);
        button(card, true);
        require(until([&] { return host.contains("recycle " + data.x + " failed"); }) && IsWindowVisible(card),
                "Failed recycle closed its Floating window");
        const auto after_failure = rect_of(card);
        require(EqualRect(&before_close, &after_failure), "Failed recycle moved the card");
        brief(std::chrono::milliseconds(GetDoubleClickTime()+20));
        button(card, true);
        require(until([&] { return !IsWindow(card); }), "Recycled card kept its Floating window open");
        clean_stop(host);
        HistoryStore store(profile);
        const auto recycled = store.hanging(domain::rebin_id);
        require(recycled.size() == 1 && recycled.front().id == data.x && recycled.front().snapshot_id == data.snapshot &&
                store.floating().empty() && store.hanging("main").size() == 99 && store.hanging(collector).empty() &&
                store.active_rope() == collector && rendering::CardBrowser(persistence::Browser(store)).summary(data.x).style == style &&
                sequence == GetClipboardSequenceNumber(),
                "Recycle lost the card, selected a different collector or changed the clipboard");
    }
    std::cout << "Floating " << data.x << " snapshot " << data.snapshot << " originally at 100 capacity; victim "
              << data.tail << " refill " << replacement << " restored at " << moved.left << ',' << moved.top
              << "; counts 100/99/100/100/99; cancel, rollback, button, drop and recycle verified\n";
}

void resize_roundtrip(const fs::path& exe, const fs::path& profile) {
    const auto data = seed(profile, 4, true);
    {
        HistoryStore store(profile);
        store.save_rope_compact("main", true, {});
    }
    const auto clipboard = GetClipboardSequenceNumber();
    {
        Desktop host(exe, profile, true, L"resize");
        const auto window = floating(host);
        const auto initial = rect_of(window);
        const int delta = MulDiv(120, GetDpiForWindow(window), 96);
        const int x = initial.right - initial.left - 2, y = (initial.bottom - initial.top) / 2;
        auto drag_border = [&] {
            SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
            SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + delta, y));
            SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(x + delta, y));
        };
        drag_border();
        const auto rollback = rect_of(window);
        require(EqualRect(&initial, &rollback), "Injected resize commit did not restore the real Floating window");
        drag_border();
        const auto changed = rect_of(window);
        require(changed.right == initial.right + delta && changed.bottom == initial.bottom,
                "Real Floating resize did not commit the width by itself");
        require(GetClipboardSequenceNumber() == clipboard, "Resizing changed the clipboard");
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(20, 18));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(20, 18));
        const int expected = MulDiv(129, GetDpiForWindow(host.workspace), 96);
        require(until([&] { return !IsWindow(window); }), "Resized card did not return to the collector");
        const auto hanging = host.card(data.x);
        require(std::abs(hanging.right - hanging.left - expected) <= 1,
                "Compact rehang did not apply the shared 150 percent width");
    }
    HistoryStore reopened(profile);
    const auto card = rendering::CardBrowser(persistence::Browser(reopened)).summary(data.x);
    require(card.entry.rope_id == "main" && card.scale == domain::CardScale{1.5f, 1} &&
            clipboard_text_is(sample::text(false)), "Reopen lost the shared scale or the returned head");
}

void edit_cases(const fs::path& exe, const fs::path& root, Fields& fields) {
    for (const bool visible : {false, true}) {
        const auto profile = root / (visible ? "visible-edit" : "hidden-edit");
        const auto data = seed(profile, 3, true);
        std::string sibling;
        {
            HistoryStore store(profile);
            sibling = store.copy(data.x, store.create_rope("Other", 2));
            if (visible)
                exec(store.database().handle(), "CREATE TRIGGER fail_edit BEFORE UPDATE OF snapshot_id ON entries "
                    "BEGIN SELECT RAISE(ABORT,'fixture write failure'); END");
        }
        Desktop host(exe, profile, visible);
        const auto card = floating(host);
        const auto original = rect_of(card);
        const auto sequence = GetClipboardSequenceNumber();
        auto begin = [&] {
            reset_fields(fields);
            brief(std::chrono::milliseconds(GetDoubleClickTime() + 20));
            input_click(card, original, fields.window);
            HWND edit{};
            require(until([&] {
                DWORD pid{};
                GetWindowThreadProcessId(card, &pid);
                const auto host_window = owned_window(pid, L"ClipRope.InlineEditorHost");
                edit = GetDlgItem(host_window, 1760);
                GUITHREADINFO info{sizeof info};
                return edit && GetGUIThreadInfo(GetWindowThreadProcessId(card, nullptr), &info) &&
                       info.hwndFocus == edit && GetForegroundWindow() == GetParent(edit);
            }), "Double-click did not focus an inline editor");
            require(GetDlgItem(GetParent(edit), IDOK) && GetDlgItem(GetParent(edit), IDCANCEL) &&
                    FindWindowExW(GetParent(edit), nullptr, L"Static", L"Saves as plain text"), "Missing inline editing controls");
            return edit;
        };
        auto click_control = [&](HWND child) {
            aim(child, center(rect_of(child)));
            mouse(MOUSEEVENTF_LEFTDOWN); mouse(MOUSEEVENTF_LEFTUP);
        };
        auto edit = begin();
        click_control(GetDlgItem(GetParent(edit), IDOK));
        require(until([&] { return !IsWindowVisible(edit); }), "Unchanged rich edit did not close");
        const auto first_field = edit;
        edit = begin();  // Rich notice must still be present after the unchanged save.
        require(edit == first_field && !SendMessageW(edit, EM_CANUNDO, 0, 0), "Rebinding leaked an undo session");
        require(SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(L"discard")), "Cannot enter draft");
        click_control(GetDlgItem(GetParent(edit), IDCANCEL));
        require(until([&] { return !IsWindowVisible(edit); }), "Cancel button did not close editor");
        edit = begin();
        const std::wstring first = L"\u7b2c\u4e00\u884c";
        const std::wstring second = L"\u7b2c\u4e8c\u884c";
        const auto expected = first + L"\r\n" + second;
        require(SendMessageW(edit, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(first.c_str())), "Cannot enter Unicode");
        SendMessageW(edit, EM_SETSEL, first.size(), first.size());
        key(VK_RETURN,true); key(VK_RETURN,false);
        require(until([&] { return SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0) == LRESULT(first.size()+2); }),
                "Enter did not insert a newline");
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(second.c_str()));
        key(VK_CONTROL, true); key('Z', true); key('Z', false); key(VK_CONTROL, false);
        require(until([&] { return SendMessageW(edit, WM_GETTEXTLENGTH, 0, 0) == LRESULT(first.size()+2); }),
                "Native undo did not remove the last edit");
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(second.c_str()));
        reset_fields(fields);
        require(IsWindowVisible(edit), "Focus loss discarded the draft");
        click_control(edit);
        key(VK_CONTROL, true); key('S',true); key('S',false); key(VK_CONTROL,false);
        if (visible) {
            require(until([&] {
                return FindWindowExW(GetParent(edit), nullptr, L"Static", L"Save failed. Draft kept.") != nullptr;
            }), "Failed save did not explain the failure in the card");
            wchar_t contents[64]{};
            SendMessageW(edit, WM_GETTEXT, 64, reinterpret_cast<LPARAM>(contents));
            require(expected == contents, "Failed save lost draft");
            click_control(edit);
            key(VK_ESCAPE, true); key(VK_ESCAPE,false);
        }
        require(until([&] { return !IsWindowVisible(edit); }), "Editing did not return to the card preview");
        const auto actual = rect_of(card);
        require(EqualRect(&original, &actual) && IsWindowVisible(card) &&
                static_cast<bool>(IsWindowVisible(host.workspace)) == visible && order(profile) == data.initial &&
                GetClipboardSequenceNumber() == sequence, "Editing changed window, clipboard or full rope");
        require(!fields.get(Query::changes_a) && !fields.get(Query::changes_b) && fields.get(Query::a_unchanged) &&
                SendDlgItemMessageW(fields.window, 1, EM_GETSEL, 0, 0) == MAKELONG(4, 5),
                "Editing delivered text to an external field");
        clean_stop(host);
        HistoryStore reopened(profile);
        const auto entry = reopened.entry(data.x);
        require(!entry.rope_id && !entry.sort_key && reopened.entry(sibling).snapshot_id == data.snapshot,
                "Editing changed membership or the shared sibling");
        const auto saved = reopened.begin_text_edit(data.x);
        require(visible ? entry.snapshot_id == data.snapshot && saved.rich :
                entry.snapshot_id != data.snapshot && !saved.rich && saved.text == expected,
                "Reopened edit did not match the save/cancel result");
    }
}
}

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--fields") return fixture::run_fields(wcstoul(argv[2], nullptr, 10));
    if (argc != 3 || std::wstring_view(argv[2]) != L"--desktop-input" || FAILED(OleInitialize(nullptr))) return 2;
    POINT cursor{};
    const bool restore = GetCursorPos(&cursor) != FALSE;
    int result = 0;
    try {
        history_fixture::Directory tmp;
        LandingPad pad;
        Fields fields;
        fields.start();
        resize_roundtrip(argv[1], tmp.path / "resize");
        lifecycle(argv[1], tmp.path / "lifecycle", fields, pad);
        edit_cases(argv[1], tmp.path, fields);
    } catch (const std::exception& problem) {
        std::cerr << problem.what() << '\n';
        result = 1;
    }
    if (restore) SetCursorPos(cursor.x, cursor.y);
    OleUninitialize();
    return result;
}
