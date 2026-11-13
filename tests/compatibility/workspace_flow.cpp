#include "persistence/hotkey_settings.hpp"
#include "platform_win/workspace_commands.hpp"
#include "platform_win/performance_counters.hpp"
#include "../integration/history_observer.hpp"
#include "desktop_app.hpp"

namespace {
using namespace workspace_fixture;

void deferred_delivery(const fs::path& exe, const fs::path& profile, Fields& fields, bool supersede) {
    domain::Id head;
    {
        HistoryStore store(profile);
        store.set_capacity("main", 2);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = sample::formats(true).front().bytes;
        store.admit("main", {{text}});
        text.bytes = sample::formats(false).front().bytes;
        head = store.admit("main", {{text}});
    }
    Desktop host(exe, profile, true);
    reset_fields(fields);
    const auto card = host.card(head);
    DWORD pid{};
    GetWindowThreadProcessId(host.workspace, &pid);
    const auto listener = owned_window(pid, L"ClipRope.Listener");
    auto db = history_fixture::observe(profile, SQLITE_OPEN_READWRITE);
    Transaction hold(db.get());
    ClipboardPublisher source;
    require(SUCCEEDED(source.publish(sample::formats(true))), "External copy failed");
    require(until([&] {
        return request(listener, win::performance_query, WPARAM(win::PerformanceCounter::capture_writing)) != 0;
    }), "Writer did not accept the external copy");
    input_click(host.workspace, card, fields.window);
    require(until([&] { return host.contains("deferred double-click " + head); }),
            "Gesture did not confirm against the pending write");
    if (supersede) require(SUCCEEDED(source.publish({{CF_UNICODETEXT, {'Y',0,0,0}}})), "New external copy failed");
    const auto before = GetClipboardSequenceNumber();
    const auto owner = GetClipboardOwner();
    require(!fields.get(Query::changes_a) && !fields.get(Query::changes_b), "Deferred recall dispatched early");
    hold.commit();
    if (supersede) {
        require(until([&] {
            const auto cards = order(profile);
            return host.contains("invoke double-click " + head + " failed skipped skipped") &&
                   cards.size() == 2 && std::find(cards.begin(), cards.end(), head) == cards.end();
        }) && IsWindowVisible(host.workspace) && fields.get(Query::a_unchanged) &&
            !fields.get(Query::changes_a) && !fields.get(Query::changes_b) && owner == GetClipboardOwner() &&
            before == GetClipboardSequenceNumber() && clipboard_text_is(L"Y"), "Superseded recall reached the editor");
    } else {
        require(until([&] { return host.contains("invoke double-click " + head + " published shortcut_sent committed") &&
                                  fields.get(Query::a_replaced); }) && fields.get(Query::changes_a) == 1 &&
                !fields.get(Query::changes_b) && order(profile).front() == head &&
                !IsWindowVisible(host.workspace), "Original editor did not consume exactly one deferred B");
    }
    std::cout << "deferred native recall superseded=" << supersede << " receiver changes=" << fields.get(Query::changes_a)
              << " other-field=" << fields.get(Query::changes_b) << " head=" << head << '\n';
}

void drag(Desktop& host, const std::string& from, const std::string& to, bool cancel) {
    POINT start = center(host.card(from)), end = center(host.card(to));
    end.x += 10; // Cross the last neighbor's center to request the tail slot.
    require(WindowFromPoint(end) == host.workspace, "Drag destination is covered");
    aim(host.workspace, start);
    mouse(MOUSEEVENTF_LEFTDOWN);
    try {
        brief(25ms);
        require(SetCursorPos(end.x, end.y) != FALSE, "Could not move captured pointer");
        brief(70ms);
        if (cancel) { key(VK_ESCAPE, true); brief(100ms); key(VK_ESCAPE, false); }
    } catch (...) {
        mouse(MOUSEEVENTF_LEFTUP);
        throw;
    }
    mouse(MOUSEEVENTF_LEFTUP);
    brief(110ms);
}
void seed(const fs::path& profile) {
    HistoryStore store(profile);
    store.set_capacity(store.active_rope(), 3);
    save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
}

void paste_into_second_field(const Fields& fields, wchar_t expected) {
    fields.act(Action::focus_b);
    const HWND editor = GetDlgItem(fields.window, 2);
    require(until([&] {
        GUITHREADINFO focus{sizeof focus};
        return GetForegroundWindow() == fields.window && GetGUIThreadInfo(0, &focus) && focus.hwndFocus == editor &&
               !(GetAsyncKeyState(VK_CONTROL) & 0x8000) && !(GetAsyncKeyState(VK_MENU) & 0x8000) &&
               !(GetAsyncKeyState(VK_SHIFT) & 0x8000);
    }), "Manual paste field or keyboard was not ready");
    const auto before = GetClipboardSequenceNumber();
    input_paste(fields.window);
    if (!until([&] {
        wchar_t value[16]{};
        DWORD_PTR length{};
        return SendMessageTimeoutW(editor, WM_GETTEXT, std::size(value), reinterpret_cast<LPARAM>(value),
                                   SMTO_ABORTIFHUNG, 300, &length) && length == 1 && value[0] == expected;
    })) {
        const wchar_t expected_text[]{expected, 0};
        std::cerr << "Manual paste of " << static_cast<char>(expected)
                  << ": sequence stable=" << (before == GetClipboardSequenceNumber())
                  << ", expected clipboard=" << clipboard_text_is(expected_text)
                  << ", field changes=" << fields.get(Query::changes_b)
                  << ", foreground retained=" << (GetForegroundWindow() == fields.window) << '\n';
        throw std::runtime_error("Manual Ctrl+V did not insert the expected letter");
    }
}

void dismiss_card_menu(Desktop& host, const std::string& id, const Fields& fields, const fs::path& profile) {
    const DWORD seq = GetClipboardSequenceNumber();
    const auto before = order(profile);
    const auto point = center(host.card(id));
    aim(host.workspace, point);
    mouse(MOUSEEVENTF_RIGHTDOWN); mouse(MOUSEEVENTF_RIGHTUP);
    DWORD pid{};
    GetWindowThreadProcessId(host.workspace, &pid);
    HWND menu{};
    require(until([&] { menu = owned_window(pid, L"ClipRope.CardMenu"); return menu && GetForegroundWindow() == menu; }),
            "Card menu did not take focus");
    PostMessageW(menu, WM_KEYDOWN, VK_ESCAPE, 0);
    require(until([&] { return !IsWindow(menu) && !owned_window(pid, L"ClipRope.CardMenuFrame") && GetForegroundWindow() == fields.window; }),
            "Closing the menu did not return the original editor");
    require(GetClipboardSequenceNumber() == seq && order(profile) == before, "Menu changed history or clipboard");
}
void menu_focus(const fs::path& exe, const fs::path& profile, Fields& fields) {
    seed(profile);
    ClipboardPublisher producer;
    Desktop host(exe, profile, true);
    const auto id = external_copy(producer, profile, L'C');
    for (bool change_field : {false, true}) {
        reset_fields(fields);
        show(host, fields);
        host.check_display({id});
        dismiss_card_menu(host, id, fields, profile);
        const auto point = center(host.card(id));
        aim(host.workspace, point);
        const auto spacing = std::chrono::milliseconds(std::min<UINT>(60, GetDoubleClickTime() / 4));
        mouse(MOUSEEVENTF_LEFTDOWN);
        brief(20ms);
        mouse(MOUSEEVENTF_LEFTUP);
        brief(spacing);
        if (change_field) fields.act(Action::focus_b);
        require(GetForegroundWindow() == fields.window && WindowFromPoint(point) == host.workspace,
                "Menu return lost the second press");
        mouse(MOUSEEVENTF_LEFTDOWN); mouse(MOUSEEVENTF_LEFTUP);
        require(until([&] { return !IsWindowVisible(host.workspace); }), "Post-menu double-click did not commit");
        if (change_field)
            require(until([&] { return host.contains("published lost-target committed"); }) && fields.get(Query::a_unchanged) &&
                    !fields.get(Query::changes_a) && !fields.get(Query::changes_b), "Changed menu target received input");
        else
            require(until([&] { return fields.get(Query::a_replaced) != 0; }) && fields.get(Query::changes_a) == 1 &&
                    !fields.get(Query::changes_b), "Original menu target did not receive exactly one edit");
    }
}
void overflow(const fs::path& exe, const fs::path& profile, Fields& fields, bool reorder) {
    seed(profile);
    ClipboardPublisher producer;
    Desktop host(exe, profile);
    require(!IsWindowVisible(host.workspace), "Host did not start tray-resident");
    const auto c = external_copy(producer, profile, L'C');
    const auto b = external_copy(producer, profile, L'B');
    const auto a = external_copy(producer, profile, L'A');
    reset_fields(fields);
    show(host, fields);
    host.check_display({a,b,c});
    if (reorder) {
        const DWORD sequence = GetClipboardSequenceNumber();
        drag(host, a, c, true);
        host.check_display({a,b,c});
        require(GetClipboardSequenceNumber() == sequence, "Canceled reorder published a preview");
        brief(std::chrono::milliseconds(GetDoubleClickTime() + 20));
        drag(host, a, c, false);
        host.check_display({b,c,a});
        require(clipboard_text_is(L"B") && !fields.get(Query::changes_a) && !fields.get(Query::changes_b),
                "Reorder did not copy the new head exactly for manual paste");
        paste_into_second_field(fields, L'B');
        reset_fields(fields);
    } else {
        input_click(host.workspace, host.card(c), fields.window);
        require(until([&] { return !IsWindowVisible(host.workspace) && order(profile) == Order{c,a,b}; }),
                "Recall did not promote C and collapse the workspace");
        require(until([&] { return fields.get(Query::a_replaced) != 0; }) && fields.get(Query::changes_a) == 1 && !fields.get(Query::changes_b),
                "Selected field did not receive exactly one insertion");
    }
    const auto d = external_copy(producer, profile, L'D');
    const Order expected = reorder ? Order{d,b,c} : Order{d,c,a};
    paste_into_second_field(fields, L'D');
    show(host, fields);
    host.check_display(expected);
    require(order(profile) == expected, "Overflow victim was incorrect");
    host.stop();
    require(host.clean_exit, "Desktop did not exit cleanly");
    Desktop restart(exe, profile, true);
    restart.check_display(expected);
    require(order(profile) == expected, "Restart changed the committed order");
    std::cout << (reorder ? "drag D,B,C" : "recall D,C,A") << " verified on screen and after restart\n";
    if (reorder) {
        const auto rect = restart.card(c);
        const auto dpi = GetDpiForWindow(restart.workspace);
        POINT close{rect.right - MulDiv(13, dpi, 96), rect.top + MulDiv(12, dpi, 96)};
        aim(restart.workspace, close);
        mouse(MOUSEEVENTF_LEFTDOWN); mouse(MOUSEEVENTF_LEFTUP);
        require(until([&] { return order(profile) == Order{d,b}; }), "Delete control did not delete C");
        restart.stop();
        Desktop after_delete(exe, profile, true);
        after_delete.check_display({d,b});
    }
}

void delivery(const fs::path& exe, const fs::path& profile, Fields& fields) {
    seed(profile);
    ClipboardPublisher producer;
    std::string c;
    {
        Desktop host(exe, profile, true);
        c = external_copy(producer, profile, L'C');
        reset_fields(fields);
        host.check_display({c});
        SendMessageW(host.workspace, WM_COMMAND, static_cast<WPARAM>(cliprope::win::WorkspaceCommand::head), 0);
        require(until([&] { return host.contains("invoke head " + c + " published skipped committed"); }),
                "Rope head did not publish the intended card");
        require(fields.get(Query::a_unchanged) && !fields.get(Query::changes_b),
                "Rope head inserted into a selected field");
        fields.act(Action::focus_button);
        input_click(host.workspace, host.card(c), fields.window);
        require(until([&] { return !IsWindowVisible(host.workspace); }), "No-field recall did not collapse");
        require(fields.get(Query::a_unchanged) && !fields.get(Query::changes_a) && !fields.get(Query::changes_b),
                "No-field activation edited a field");
        fields.act(Action::focus_b);
        const auto manual_sequence = GetClipboardSequenceNumber();
        input_paste(fields.window);
        if (!until([&] { return fields.get(Query::b_pasted) != 0; })) {
            std::cerr << "Manual paste sequence-stable=" << (GetClipboardSequenceNumber() == manual_sequence)
                      << " fixture-foreground=" << (GetForegroundWindow() == fields.window)
                      << " field-edits=" << fields.get(Query::changes_b) << '\n';
            throw std::runtime_error("Subsequent native Ctrl+V did not paste C");
        }
        require(fields.get(Query::changes_b) == 1, "Native Ctrl+V edited the field more than once");
        reset_fields(fields);
        show(host, fields);
        fields.act(Action::suppress_after);
        brief(std::chrono::milliseconds(GetDoubleClickTime() + 20));
        input_click(host.workspace, host.card(c), fields.window);
        require(until([&] { return !IsWindowVisible(host.workspace); }), "Unknown delivery prevented a history commit");
        require(until([&] { return fields.get(Query::changes_a) == 1; }) && fields.get(Query::edit_attempts) == 1 &&
                !fields.get(Query::changes_b), "Ambiguous delivery was retried");
        reset_fields(fields);
        show(host, fields);
        brief(std::chrono::milliseconds(GetDoubleClickTime() + 20));
        aim(host.workspace, center(host.card(c)));
        mouse(MOUSEEVENTF_LEFTDOWN); mouse(MOUSEEVENTF_LEFTUP);
        brief(50ms);
        fields.act(Action::focus_b);
        mouse(MOUSEEVENTF_LEFTDOWN); mouse(MOUSEEVENTF_LEFTUP);
        require(until([&] { return !IsWindowVisible(host.workspace); }), "Changed-target activation did not commit");
        require(fields.get(Query::a_unchanged) && !fields.get(Query::changes_b), "Changed target was edited");
    }
    {
        HistoryStore configuration(profile);
        exec(configuration.database().handle(), "UPDATE app_settings SET paste_on_double_click=0");
    }
    {
        Desktop host(exe, profile, true);
        external_copy(producer, profile, L'B');
        reset_fields(fields);
        input_click(host.workspace, host.card(c), fields.window);
        require(until([&] { return host.contains("invoke double-click " + c + " published skipped committed"); }) &&
                !IsWindowVisible(host.workspace) && order(profile).front() == c, "Copy-only did not promote and collapse");
        require(fields.get(Query::a_unchanged) && !fields.get(Query::changes_a), "Copy-only edited the original field");
        input_paste(fields.window);
        require(until([&] { return fields.get(Query::a_replaced) && fields.get(Query::changes_a) == 1; }),
                "Manual paste after copy-only did not edit the original field once");
    }
    {
        HistoryStore configuration(profile);
        exec(configuration.database().handle(), "UPDATE app_settings SET paste_on_double_click=1");
    }
    for (const wchar_t* fault : {L"publish", L"history"}) {
        Desktop host(exe, profile, true, fault);
        external_copy(producer, profile, L'A');
        reset_fields(fields);
        const auto previous = order(profile);
        const DWORD sequence = GetClipboardSequenceNumber();
        input_click(host.workspace, host.card(c), fields.window);
        require(until([&] { return host.contains("invoke "); }), "No invocation result for injected fault");
        require(IsWindowVisible(host.workspace) && order(profile) == previous, "Failed activation changed durable history or hid the window");
        const bool after_publish = std::wstring_view(fault) == L"history";
        require(until([&] { return fields.get(Query::changes_a) == static_cast<unsigned>(after_publish); }) && !fields.get(Query::changes_b),
                "Injected fault produced an unexpected delivery result");
        require((GetClipboardSequenceNumber() != sequence) == after_publish, "Injected fault crossed the wrong publication boundary");
    }
    {
        Desktop host(exe, profile, true, L"replace-before-delivery");
        reset_fields(fields);
        input_click(host.workspace, host.card(c), fields.window);
        require(until([&] { return host.contains("probe awaiting clipboard replacement"); }), "Publication not observed");
        require(until([&] { return request(fields.window, fixture::command, static_cast<WPARAM>(Action::overwrite)) == 1; }),
                "External fixture could not take clipboard ownership");
        require(until([&] { return host.contains("invoke double-click " + c + " published replaced committed"); }),
                "A newer clipboard owner did not cancel delivery");
        require(fields.get(Query::a_unchanged) && !fields.get(Query::changes_b), "An obsolete publication edited a field");
    }
    std::cout << "Manual paste, lost target, uncertain acknowledgement, publish and commit failure passed\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--fields") return fixture::run_fields(wcstoul(argv[2], nullptr, 10));
    const bool menus = argc == 3 && std::wstring_view(argv[2]) == L"--menu-focus";
    const bool follow = argc == 3 && std::wstring_view(argv[2]) == L"--follow-head";
    const bool deferred = argc == 3 && std::wstring_view(argv[2]) == L"--deferred-recall";
    if (argc != 3 || (!menus && !follow && !deferred && std::wstring_view(argv[2]) != L"--desktop-input") || FAILED(OleInitialize(nullptr))) return 2;
    POINT cursor{};
    const bool restore = GetCursorPos(&cursor) != FALSE;
    int result = 0;
    try {
        history_fixture::Directory temporary;
        Fields fields;
        fields.start();
        if (deferred) {
            deferred_delivery(argv[1], temporary.path / "accepted", fields, false);
            deferred_delivery(argv[1], temporary.path / "replaced", fields, true);
        } else if (menus) menu_focus(argv[1], temporary.path / "menu", fields);
        else if (follow) overflow(argv[1], temporary.path / "reorder", fields, true);
        else {
            overflow(argv[1], temporary.path / "promote", fields, false);
            overflow(argv[1], temporary.path / "reorder", fields, true);
            delivery(argv[1], temporary.path / "delivery", fields);
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    if (restore) SetCursorPos(cursor.x, cursor.y);
    OleUninitialize();
    return result;
}
