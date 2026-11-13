#include "persistence/hotkey_settings.hpp"
#include "platform_win/workspace_commands.hpp"
#include "desktop_app.hpp"

namespace {
using namespace workspace_fixture;

domain::Snapshot sample_for(std::string_view label, bool rich) {
    domain::Snapshot result;
    for (const auto& item : sample::formats(false)) {
        if (!rich && item.format != CF_UNICODETEXT) continue;
        domain::Format value;
        if (item.format == CF_UNICODETEXT) value.standard_id = CF_UNICODETEXT;
        else value.name = "HTML Format";
        value.bytes = item.bytes;
        if (!rich) {
            value.bytes.clear();
            for (const auto ch : label) { value.bytes.push_back(static_cast<unsigned char>(ch)); value.bytes.push_back(0); }
            value.bytes.insert(value.bytes.end(), {0,0});
        }
        result.formats.push_back(std::move(value));
    }
    return result;
}

struct Profile {
    std::string a = "main", b, c, y_snapshot;
    Order before_a, before_b, before_c;
    application::CardStyle style{18,true,0x152c24,0xf2e4cc,1.0};
    explicit Profile(const fs::path& folder) {
        HistoryStore store(folder);
        store.set_capacity(a, 100);
        b = store.create_rope("B", 100);
        c = store.create_rope("C", 100);
        store.save_rope_compact(b, true, {});
        for (const auto& [id, name] : {std::pair{a,"A"}, std::pair{b,"B"}, std::pair{c,"C"}})
            for (int i = 100; i > 0; --i)
                store.admit(id, sample_for(name + std::to_string(i), id == a && i == 2));
        store.set_active_rope(a);
        save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
        before_a = order(folder, a); before_b = order(folder, b); before_c = order(folder, c);
        require(before_a.size() == 100 && before_b.size() == 100 && before_c.size() == 100, "Three full ropes required");
        y_snapshot = store.entry(before_a[1]).snapshot_id;
        store.save_style(before_a[1], style);
    }
};

void overview(Desktop& host) {
    if (host.frame().rows.size() >= 3) return;
    SendMessageW(host.workspace, WM_COMMAND, static_cast<WPARAM>(cliprope::win::WorkspaceCommand::overview), 0);
    require(until([&] { return host.frame().rows.size() >= 3; }), "Three visible overview rows unavailable");
}

void transfer(Desktop& host, const std::string& source, const std::string& destination_card, bool copy) {
    const auto start = center(host.card(source));
    const auto dest = host.card(destination_card);
    const POINT end{dest.left + 12, center(dest).y};
    require(WindowFromPoint(end) == host.workspace, "Destination card is covered");
    aim(host.workspace, start);
    if (copy) key(VK_CONTROL, true);
    mouse(MOUSEEVENTF_LEFTDOWN);
    try {
        brief(25ms);
        require(SetCursorPos(end.x, end.y) != FALSE, "Cross-rope pointer motion failed");
        brief(115ms);
    } catch (...) {
        mouse(MOUSEEVENTF_LEFTUP);
        if (copy) key(VK_CONTROL, false);
        throw;
    }
    mouse(MOUSEEVENTF_LEFTUP);
    brief(90ms);
    if (copy) key(VK_CONTROL, false);
}

void clean_stop(Desktop& host) {
    host.stop();
    require(host.clean_exit, "Desktop host did not exit cleanly");
}

void verify_recalled_clipboard(HWND workspace) {
    DWORD host_pid{};
    GetWindowThreadProcessId(workspace, &host_pid);
    require(until([] { return OpenClipboard(nullptr) != FALSE; }), "Clipboard stayed busy before manual Ctrl+V");
    struct Unlock { ~Unlock() { CloseClipboard(); } } unlock;
    DWORD owner_pid{};
    GetWindowThreadProcessId(GetClipboardOwner(), &owner_pid);
    require(host_pid && owner_pid == host_pid, "Another source replaced the recalled clipboard");
    for (const auto& item : sample::formats(false)) {
        auto memory = GetClipboardData(item.format);
        require(memory && GlobalSize(memory) >= item.bytes.size(), "Recalled clipboard format is unavailable");
        auto bytes = GlobalLock(memory);
        require(bytes != nullptr, "Recalled clipboard format could not be read");
        const bool same = std::memcmp(bytes, item.bytes.data(), item.bytes.size()) == 0;
        GlobalUnlock(memory);
        require(same, "Recalled clipboard format differs from the original sibling");
    }
}

void run(const fs::path& exe, const fs::path& root, Fields& fields) {
    Profile data(root);
    const auto x = data.before_a[0], y = data.before_a[1];
    const auto b_tail = data.before_b.back(), c_tail = data.before_c.back();
    {
        Desktop host(exe, root, true, L"transfer");
        reset_fields(fields);
        overview(host);
        const auto regular = host.card(x), compact = host.card(data.before_b[0]);
        require(compact.right - compact.left < regular.right - regular.left,
                "Transfer fixture did not restore distinct rope densities");
        const DWORD version = GetClipboardSequenceNumber();
        transfer(host, x, data.before_b[2], false);
        require(until([&] { return host.contains("transfer move " + x + " " + data.b + " failed"); }),
                "Injected transfer failure did not reach the command boundary");
        require(GetClipboardSequenceNumber() == version && fields.get(Query::changes_a) == 0,
                "Failed transfer touched clipboard or selection");
        clean_stop(host);
    }
    require(order(root, data.a) == data.before_a && order(root, data.b) == data.before_b &&
            order(root, data.c) == data.before_c, "Failed transfer changed reopened orders");
    Order a = data.before_a, b = data.before_b, c = data.before_c;
    a.erase(a.begin()); b.pop_back(); b.insert(b.begin() + 2, x);
    std::string copy_id;
    {
        Desktop host(exe, root, true);
        reset_fields(fields);
        overview(host);
        auto sequence = GetClipboardSequenceNumber();
        transfer(host, x, data.before_b[2], false);
        require(until([&] {
            return host.contains("transfer move " + x + " " + data.b + " committed " + x);
        }), "Cross-rope move did not commit");
        require(order(root, data.a) == a && order(root, data.b) == b && order(root, data.c) == c,
                "Move did not preserve the two exact orders with target-only eviction");
        require(sequence != GetClipboardSequenceNumber(), "Moving the active head did not publish its successor");
        verify_recalled_clipboard(host.workspace);
        sequence = GetClipboardSequenceNumber();
        transfer(host, y, data.before_c[1], true);
        require(until([&] { return host.contains("transfer copy " + y + " " + data.c + " committed "); }),
                "Ctrl+drag copy did not commit");
        copy_id = order(root, data.c).at(1);
        c.pop_back(); c.insert(c.begin() + 1, copy_id);
        require(copy_id != y && order(root, data.a) == a && order(root, data.b) == b && order(root, data.c) == c,
                "Copy did not preserve the source and evict exactly the target tail");
        require(GetClipboardSequenceNumber() == sequence && fields.get(Query::changes_a) == 0,
                "Transfer gestures altered the clipboard or selected field");
        clean_stop(host);
    }
    {
        HistoryStore store(root);
        Browser browser(store);
        require(store.entry(x).rope_id == data.b && store.entry(copy_id).snapshot_id == data.y_snapshot &&
                store.entry(y).snapshot_id == data.y_snapshot && browser.summary(copy_id).style == data.style,
                "Copy lost shared content or source appearance on restart");
        require(store.active_rope() == data.a, "Dragging between overview rows changed the collector");
    }
    {
        Desktop host(exe, root, true);
        reset_fields(fields);
        overview(host);
        const auto r = host.card(copy_id);
        const auto dpi = GetDpiForWindow(host.workspace);
        POINT close{r.right-MulDiv(13,dpi,96),r.top+MulDiv(12,dpi,96)};
        aim(host.workspace, close);
        mouse(MOUSEEVENTF_LEFTDOWN); brief(50ms); mouse(MOUSEEVENTF_LEFTUP);
        c.erase(c.begin() + 1);
        require(until([&] { return order(root, data.c) == c; }), "Copy delete did not preserve target order");
        clean_stop(host);
    }
    {
        HistoryStore store(root);
        store.gc();
        const auto content = store.load(y);
        require(content.snapshot.formats.size() == 2 && content.snapshot.formats[1].bytes == sample::formats(false)[1].bytes,
                "Collection after deleting a copy damaged its sibling's HTML");
    }
    {
        Desktop host(exe, root, true);
        reset_fields(fields);
        fields.act(Action::focus_button);
        input_click(host.workspace,host.card(y),fields.window);
        require(until([&] { return !IsWindowVisible(host.workspace) &&
            host.contains("invoke double-click " + y + " published no-field committed"); }), "Post-GC sibling recall failed");
        fields.act(Action::focus_b);
        verify_recalled_clipboard(host.workspace);
        GUITHREADINFO focus{sizeof(focus)};
        require(GetGUIThreadInfo(0, &focus) && focus.hwndFocus == GetDlgItem(fields.window, 2), "Field B lost focus");
        const auto sequence = GetClipboardSequenceNumber();
        input_paste(fields.window);
        if (!until([&] { return fields.get(Query::b_pasted) != 0; })) {
            std::cerr << "post-paste sequence-stable=" << (GetClipboardSequenceNumber() == sequence)
                      << " fixture-foreground=" << (GetForegroundWindow() == fields.window)
                      << " field-A-edits=" << fields.get(Query::changes_a) << " field-B-edits=" << fields.get(Query::changes_b) << '\n';
            throw std::runtime_error("Post-GC sibling bytes did not paste");
        }
        require(fields.get(Query::changes_b) == 1 && !fields.get(Query::changes_a), "Manual paste edited an unexpected field");
        clean_stop(host);
    }
    std::cout << "collector=" << data.a << " targets=" << data.b << "," << data.c << " move=" << x
              << " source=" << y << " copy=" << copy_id << " shared=" << data.y_snapshot
              << " victims=" << b_tail << "," << c_tail << " counts=99,100,100; rollback, restart and GC passed\n";
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (argc == 3 && std::wstring_view(argv[1]) == L"--fields") return fixture::run_fields(wcstoul(argv[2], nullptr, 10));
    if (argc != 3 || std::wstring_view(argv[2]) != L"--desktop-input" || FAILED(OleInitialize(nullptr))) return 2;
    POINT cursor{};
    const bool restore = GetCursorPos(&cursor) != FALSE;
    int result = 0;
    try {
        history_fixture::Directory temporary;
        Fields fields;
        fields.start();
        run(argv[1],temporary.path,fields);
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    if (restore) SetCursorPos(cursor.x,cursor.y);
    OleUninitialize();
    return result;
}
