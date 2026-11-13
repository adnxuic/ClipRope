#include "desktop_app.hpp"
#include "../integration/resident.hpp"
#include "persistence/hotkey_settings.hpp"
#include "platform_win/workspace_commands.hpp"
#include "presentation/hanging_layout.hpp"

namespace {
using namespace workspace_fixture;
class Observe {
public:
    explicit Observe(const fs::path &root) {
        auto name = (root / "history.db").u8string();
        require(sqlite3_open_v2(reinterpret_cast<const char *>(name.c_str()), &db_, SQLITE_OPEN_READONLY,
                                nullptr) == SQLITE_OK,
                "Could not open read-only fixture observer");
        sqlite3_busy_timeout(db_, 500);
    }
    ~Observe() {
        sqlite3_close(db_);
    }
    std::string scalar(const char *sql, std::string_view arg = {}) {
        Statement q(db_, sql);
        if (!arg.empty())
            q.bind(1, arg);
        return q.step() ? q.text(0) : "";
    }
    std::string active() {
        return scalar("SELECT active_rope_id FROM app_settings");
    }
    Order cards(std::string_view rope) {
        Statement q(db_, "SELECT id FROM entries WHERE rope_id=? ORDER BY sort_key,id");
        q.bind(1, rope);
        Order result;
        while (q.step())
            result.push_back(q.text(0));
        return result;
    }
    std::string state() {
        return scalar("SELECT group_concat(quote(id)||quote(name)||quote(color_value)||quote(capacity)) FROM "
                      "(SELECT "
                      "* FROM ropes ORDER BY id)") +
               scalar("SELECT group_concat(quote(id)||quote(rope_id)||quote(sort_key)||quote(snapshot_id)) "
                      "FROM (SELECT * FROM entries ORDER BY id)") +
               active() + scalar("SELECT default_capacity FROM app_settings");
    }

private:
    sqlite3 *db_{};
};
void click(HWND target, POINT point, bool right = false) {
    aim(target, point);
    mouse(right ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN);
    mouse(right ? MOUSEEVENTF_RIGHTUP : MOUSEEVENTF_LEFTUP);
}
DWORD pid(HWND h) {
    DWORD value{};
    GetWindowThreadProcessId(h, &value);
    return value;
}
struct MenuExpectation { bool overview, compact, cards, protected_rope, multiple, rebin_visible = false; };
HWND action(Desktop &app, UINT command, bool opens = true, std::optional<POINT> point = {},
            std::optional<MenuExpectation> expected = {}) {
    click(app.workspace, point ? *point : single_rope_point(app), true);
    HWND popup{};
    require(until([&] {
                popup = owned_window(pid(app.workspace), L"ClipRope.RopeMenu");
                return popup && IsWindowVisible(popup);
            }),
            "Rope line did not open its menu");
    const UINT ids[] = {106, UINT(win::WorkspaceCommand::latest), UINT(win::WorkspaceCommand::density),
                        102, 103, UINT(win::WorkspaceCommand::rebin), UINT(win::WorkspaceCommand::overview), 105, 108, UINT(win::WorkspaceCommand::find), 110};
    const wchar_t* labels[] = {L"Edit style…", L"Back to first card", L"Card density", L"Previous rope", L"Next rope",
                              L"Show / hide ReBin", L"Rope overview", L"Add new rope…", L"Clear Hanging cards…", L"Find in Cards", L"Delete this rope…"};
    size_t count = 0;
    for(auto ctrl=GetWindow(popup,GW_CHILD);ctrl;ctrl=GetWindow(ctrl,GW_HWNDNEXT)) {
        const int id=GetDlgCtrlID(ctrl);
        if(id>=1901 && id<=1903 || id>=9000) continue;
        require(count<std::size(ids) && id==int(ids[count]),"Rope menu has an extra or reordered action");
        wchar_t buffer[128]{};
        SendMessageW(ctrl,WM_GETTEXT,std::size(buffer),LPARAM(buffer));
        std::wstring label(buffer);
        std::erase(label, L'&');
        require(label == labels[count], "Unexpected rope menu label");
        if (expected) {
            const bool disabled = ((count == 1 || count == 8) && !expected->cards) ||
                                  ((count == 3 || count == 4) && !expected->multiple) || (count == 10 && expected->protected_rope);
            require((!IsWindowEnabled(ctrl)) == disabled, "Incorrect rope menu enabled state");
        }
        ++count;
    }
    require(count == std::size(ids), "Incomplete rope menu");
    auto checked = [&](int id) { return IsDlgButtonChecked(popup, id) == BST_CHECKED; };
    constexpr int compact = 30050, all = 30090;
    require(checked(compact) != checked(compact + 1) && checked(all) != checked(all + 1),
            "Each view selector must have one checked option");
    if (expected)
        require(checked(compact) == expected->compact && checked(all) == expected->overview &&
                checked(int(win::WorkspaceCommand::rebin)) == expected->rebin_visible, "Wrong rope control state");
    if (!command) {
        key(VK_ESCAPE, true); key(VK_ESCAPE, false);
        require(until([&] { return !IsWindow(popup); }), "Rope menu failed to cancel");
        return nullptr;
    }
    auto item=GetDlgItem(popup,command);
    if (command == UINT(win::WorkspaceCommand::density) || command == UINT(win::WorkspaceCommand::overview)) {
        const int first = command == UINT(win::WorkspaceCommand::density) ? compact : all;
        const int selected = checked(first) ? first : first + 1;
        SendDlgItemMessageW(popup, selected, BM_CLICK, 0, 0);
        require(IsWindowVisible(popup) && checked(selected), "Clicking the selected option changed the mode");
        item = GetDlgItem(popup, selected == first ? first + 1 : first);
    }
    RECT rect{};
    require(item && GetWindowRect(item,&rect),"Rope command unavailable");
    click(item,center(rect));
    require(until([&] { return !IsWindow(popup) || !IsWindowVisible(popup); }), "Rope menu remained open");
    if (!opens)
        return nullptr;
    HWND dlg{};
    require(until([&] {
                dlg = owned_window(pid(app.workspace), L"ClipRope.RopeManager");
                return dlg && IsWindowVisible(dlg);
            }),
            "Rope dialog unavailable");
    return dlg;
}
std::wstring text(HWND dlg, int id) {
    wchar_t buf[512]{};
    SendDlgItemMessageW(dlg, id, WM_GETTEXT, 512, reinterpret_cast<LPARAM>(buf));
    return buf;
}
void set(HWND dlg, int id, const wchar_t *data) {
    require(SendDlgItemMessageW(dlg, id, WM_SETTEXT, 0, reinterpret_cast<LPARAM>(data)) != FALSE,
            "Could not set synthetic rope field");
}
void button(HWND dlg, int id, bool closes = true) {
    const auto ctrl = GetDlgItem(dlg, id);
    RECT r{};
    require(ctrl && GetWindowRect(ctrl, &r), "Missing dialog button");
    click(ctrl, center(r));
    if (closes)
        require(until([&] { return !IsWindow(dlg); }), "Rope manager did not finish");
}
void preview(HWND dlg, std::wstring_view expected) {
    button(dlg, IDOK, false);
    require(until([&] {
                return text(dlg, IDOK).find(L"Confirm") != std::wstring::npos &&
                       text(dlg, 1905).find(expected) != std::wstring::npos;
            }),
            "Affected count missing from confirm step");
}
template <class Prepare>
void canceled_then_committed(Desktop &app, Observe &read, int item, Prepare prepare, std::wstring_view count) {
    const auto before = read.state();
    auto dlg = action(app, item);
    prepare(dlg);
    preview(dlg, count);
    button(dlg, IDCANCEL);
    require(read.state() == before, "Cancel mutated rope or entry state");
    dlg = action(app, item);
    prepare(dlg);
    preview(dlg, count);
    button(dlg, IDOK);
}
domain::Snapshot sample(char ch) {
    domain::Format f;
    f.standard_id = 13;
    f.bytes = {static_cast<unsigned char>(ch), 0, 0, 0};
    return {{f}};
}
void move_selection(Desktop &app, Observe &read, int menu, const std::string &dest) {
    action(app, menu, false);
    require(until([&] { return read.active() == dest; }), "Collector did not switch");
}
HWND switch_question(Desktop& app) {
    HWND question{};
    require(until([&] {
        question = owned_window(pid(app.workspace), L"ClipRope.RopeManager");
        return question && text(question, IDOK) == L"&Yes";
    }), "Creation did not ask whether to switch");
    return question;
}
POINT plaque(Desktop& app, RECT row) {
    // Reported row bounds enclose the line. Its center is the curve's sag.
    auto point = center(row);
    point.y -= MulDiv(int(presentation::RopePlaque::gap + presentation::RopePlaque::height / 2),
                      GetDpiForWindow(app.workspace), 96);
    return point;
}
void failed_creation_and_selection(const fs::path& host) {
    for (const auto fault : {L"rope-create", L"rope-select"}) {
        history_fixture::Directory dir;
        { HistoryStore store(dir.path); save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"}); }
        Desktop app(host, dir.path, true, fault);
        Observe read(dir.path);
        const auto sequence = GetClipboardSequenceNumber();
        auto dialog = action(app, 105);
        set(dialog, 1901, L"Created once");
        if (std::wstring_view(fault) == L"rope-create") {
            button(dialog, IDOK, false);
            require(until([&] { return text(dialog, 1905).find(L"could not be committed") != std::wstring::npos; }) &&
                    read.active() == "main" && read.scalar("SELECT count(*) FROM ropes WHERE id<>'system:rebin'") == "1" &&
                    text(dialog, IDOK) != L"&Yes", "Failed creation changed data or opened the switch prompt");
        }
        button(dialog, IDOK);
        auto question = switch_question(app);
        if (std::wstring_view(fault) == L"rope-select") {
            button(question, IDOK, false);
            require(until([&] { return text(question, 1905).find(L"switching failed") != std::wstring::npos; }) &&
                    read.active() == "main" && read.scalar("SELECT count(*) FROM ropes WHERE id<>'system:rebin'") == "2",
                    "Failed switch changed the collector or lost the new rope");
        }
        button(question, IDOK);
        require(read.active() != "main" && read.scalar("SELECT count(*) FROM ropes WHERE id<>'system:rebin'") == "2" &&
                GetClipboardSequenceNumber() == sequence, "Retry duplicated creation or changed the clipboard");
    }
}
void run(const fs::path &host) {
    history_fixture::Directory dir;
    {
        HistoryStore s(dir.path);
        save_shortcut(s.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
        application::RopeChange defaults{application::RopeOp::defaults};
        defaults.capacity = 25;
        s.apply_rope_change(s.preview_rope_change(defaults));
        for (int i = 0; i < 20; ++i) s.admit("main", sample('M'));
    }
    struct Cursor {
        POINT p{};
        bool ok = GetCursorPos(&p) != FALSE;
        ~Cursor() {
            if (ok)
                SetCursorPos(p.x, p.y);
        }
    } cursor;
    std::string a, b, c, floating;
    {
        Desktop app(host, dir.path, true);
        Observe read(dir.path);
        const auto sequence = GetClipboardSequenceNumber();
        action(app, 0, false, {}, MenuExpectation{false, false, true, true, false});
        const auto head = read.cards("main").front();
        const auto regular = app.card(head);
        action(app, UINT(win::WorkspaceCommand::density), false);
        require(until([&] {
                    const auto compact = app.card(head);
                    return read.scalar("SELECT compact FROM rope_view_state WHERE rope_id='main'") == "1" &&
                           compact.right - compact.left < regular.right - regular.left;
                }),
                "Compact view did not persist or shrink the card");
        const auto line = single_rope_point(app);
        SendMessageW(app.workspace, WM_MOUSEWHEEL, MAKEWPARAM(0, -5 * WHEEL_DELTA), MAKELPARAM(line.x, line.y));
        require(!read.scalar("SELECT anchor_entry_id FROM rope_view_state WHERE rope_id='main'").empty(), "Wheel did not browse old cards");
        const auto before_head = read.state();
        action(app, UINT(win::WorkspaceCommand::latest), false);
        require(until([&] { return read.scalar("SELECT anchor_entry_id FROM rope_view_state WHERE rope_id='main'").empty(); }) &&
                read.state() == before_head && GetClipboardSequenceNumber() == sequence,
                "Back to first card changed history or published the clipboard");
        auto dlg = action(app, 106);
        require(text(dlg, 1901) == L"main" &&
                (GetWindowLongPtrW(GetDlgItem(dlg, 1901), GWL_STYLE) & ES_READONLY),
                "Main name is not read-only in properties");
        button(dlg, IDCANCEL);
        dlg = action(app, 105);
        button(dlg, IDCANCEL);
        require(read.scalar("SELECT count(*) FROM ropes WHERE id<>'system:rebin'") == "1", "Canceled creation added a rope");
        for (auto label : {L"A", L"B", L"C"}) {
            const auto collector = read.active();
            dlg = action(app, 105);
            require(text(dlg, 1902) == L"25", "New rope did not initialize from the saved default");
            set(dlg, 1901, label);
            if (label[0] != L'B')
                set(dlg, 1902, label[0] == L'A' ? L"3" : L"2");
            button(dlg, IDOK);
            auto question = switch_question(app);
            const std::string name(1, char(label[0]));
            auto id = read.scalar("SELECT id FROM ropes WHERE name=?", name);
            require(!id.empty() && read.active() == collector &&
                    LOWORD(SendMessageW(question, DM_GETDEFID, 0, 0)) == IDCANCEL,
                    "Creation switched before consent or defaulted to Yes");
            if (label[0] == L'A') button(question, IDOK);
            else {
                if (label[0] == L'B') button(question, IDCANCEL);
                else SendMessageW(question, WM_CLOSE, 0, 0);
                require(read.active() == collector && !read.scalar("SELECT id FROM ropes WHERE id=?", id).empty(),
                        "Declining the switch removed the rope or changed collector");
                move_selection(app, read, 103, id);
            }
            if (label[0] == L'A')
                a = id;
            else if (label[0] == L'B')
                b = id;
            else
                c = id;
        }
        require(a != b && b != c && a != c, "Rope IDs were not independent");
        action(app, 0, false, {}, MenuExpectation{false, false, false, false, true});
        const auto before_color = read.state(),
                   before_recent = read.scalar("SELECT count(*) FROM recent_colors");
        dlg = action(app, 106);
        PostMessageW(GetDlgItem(dlg, 1903), BM_CLICK, 0, 0);
        HWND staged{};
        require(until([&] {
                    staged = owned_window(pid(dlg), L"ClipRope.ColorPicker");
                    return staged && IsWindowVisible(staged) && GetForegroundWindow() == staged &&
                           GetDlgItem(staged, IDOK);
                }),
                "Staged picker did not open");
        HWND preset{};
        EnumChildWindows(
            staged,
            [](HWND h, LPARAM p) -> BOOL {
                if (GetDlgCtrlID(h) == 4104)
                    *reinterpret_cast<HWND *>(p) = h;
                return TRUE;
            },
            LPARAM(&preset));
        SendMessageW(preset, BM_CLICK, 0, 0);
        button(staged, IDOK);
        button(dlg, IDCANCEL);
        require(read.state() == before_color &&
                    read.scalar("SELECT count(*) FROM recent_colors") == before_recent,
                "Cancel of outer properties committed its staged color");
        dlg = action(app, 106);
        set(dlg, 1901, L"C renamed");
        PostMessageW(GetDlgItem(dlg, 1903), BM_CLICK, 0, 0);
        HWND picker{};
        require(until([&] {
                    picker = owned_window(pid(dlg), L"ClipRope.ColorPicker");
                    return picker && IsWindowVisible(picker) && GetForegroundWindow() == picker &&
                           GetDlgItem(picker, IDOK);
                }),
                "Properties picker did not open");
        HWND hex{};
        EnumChildWindows(
            picker,
            [](HWND h, LPARAM value) -> BOOL {
                if (GetDlgCtrlID(h) == 4150)
                    *reinterpret_cast<HWND *>(value) = h;
                return TRUE;
            },
            LPARAM(&hex));
        require(hex != nullptr, "HEX missing");
        SendMessageW(hex, WM_SETTEXT, 0, LPARAM(L"#123456"));
        button(picker, IDOK);
        require(read.scalar("SELECT color_value FROM ropes WHERE id=?", c) != "#123456FF",
                "Outer properties wrote a staged draft");
        button(dlg, IDOK);
        require(read.scalar("SELECT name FROM ropes WHERE id=?", c) == "C renamed" &&
                    read.scalar("SELECT color_value FROM ropes WHERE id=?", c) == "#123456FF",
                "Rename or recolor failed");
        move_selection(app, read, 102, b);
        require(GetClipboardSequenceNumber() == sequence, "Rope management published clipboard content");
        SendMessageW(app.workspace, WM_CLOSE, 0, 0);
        require(until([&] { return !IsWindowVisible(app.workspace); }), "Workspace did not collapse");
        ClipboardPublisher writer;
        require(SUCCEEDED(writer.publish(::sample::formats(false))), "External synthetic copy failed");
        require(until([&] { return read.cards(b).size() == 1; }, 5s) && read.cards(a).empty() &&
                    read.cards(c).empty() && read.cards("main").size() == 20,
                "Hidden capture was not exclusive to active B");
        app.stop();
        require(app.clean_exit, "First management session did not exit cleanly");
    }
    {
        HistoryStore s(dir.path);
        require(s.active_rope() == b, "Collector did not survive restart");
        require(s.rope_compact("main") && !s.rope_compact(b), "Independent densities did not survive restart");
        Browser browser(s);
        require(browser.rope(a).capacity == 3 && browser.rope(b).capacity == 25 && browser.rope(c).capacity == 2,
                "Per-rope capacities changed");
        floating = s.admit(a, sample('F'));
        s.detach(floating, {"", 600, 300, 240, 160});
        for (char ch : {'a', 'b', 'c'})
            s.admit(a, sample(ch));
        for (char ch : {'d', 'e', 'f'})
            s.admit(b, sample(ch));
        s.admit(c, sample('x'));
        s.admit(c, sample('y'));
    }
    {
        Desktop app(host, dir.path, true);
        Observe read(dir.path);
        canceled_then_committed(
            app, read, 106, [](HWND dlg) { set(dlg, 1902, L"3"); }, L"Remove 1 Hanging");
        require(read.cards(b).size() == 3, "Confirmed shrink did not remove its tail");
        move_selection(app, read, 102, a);
        const auto retained = read.cards(b);
        canceled_then_committed(
            app, read, 110, [](HWND dlg) { require(!GetDlgItem(dlg, 1904), "Delete still offers a transfer target"); }, L"Remove 3 Hanging");
        require(read.active() == "main" && read.scalar("SELECT id FROM ropes WHERE id=?", a).empty() &&
                    read.cards(b) == retained && app.floating_window(),
                "Delete did not preserve another rope and Floating or fall back to main");
        move_selection(app, read, 103, b);
        auto none = [](HWND) {};
        canceled_then_committed(app, read, 108, none, L"Remove 3 Hanging");
        require(read.cards(b).empty() && read.cards(c).size() == 2 && app.floating_window(),
                "Clear current crossed ownership");
        move_selection(app, read, 103, c);
        canceled_then_committed(app, read, 110, none, L"Remove 2 Hanging");
        require(read.active() == "main" && read.scalar("SELECT id FROM ropes WHERE id=?", c).empty() &&
                    app.floating_window(),
                "Delete history did not fall back to main");
        canceled_then_committed(app, read, 108, none, L"Remove 20 Hanging");
        require(read.cards("main").empty() && app.floating_window(), "Clear main damaged an independent card");
        action(app, 0, false, {}, MenuExpectation{false, true, false, true, true});
        action(app, 0, false, plaque(app, app.frame().rows.front().second),
               MenuExpectation{false, true, false, true, true});
        action(app, UINT(win::WorkspaceCommand::overview), false);
        require(until([&] { return app.frame().rows.size() == 2; }), "Overview did not display both ropes");
        const auto rows = app.frame().rows;
        action(app, 0, false, center(rows.front().second), MenuExpectation{true, true, false, true, true});
        action(app, 0, false, plaque(app, rows.back().second), MenuExpectation{true, false, false, false, true});
        action(app, UINT(win::WorkspaceCommand::overview), false, plaque(app, rows.back().second));
        require(until([&] { return app.frame().rows.size() == 1 && read.active() == b; }),
                "Return to this rope did not select the pointed overview row");
        move_selection(app, read, 102, "main");
        app.stop();
        require(app.clean_exit, "Management session did not exit cleanly");
    }
    HistoryStore s(dir.path);
    require(s.active_rope() == "main" && s.floating().size() == 1 && s.hanging(b).empty(),
            "Management restart mismatch");
    std::cout << "menu_actions=10 created=A,B,C switch_yes_no_close=1 active_capture=B defaults=25 "
                 "floating_survived_source_delete=1 density_restart=1 head_scroll_only=1\n";
}
void rebin_navigation(const fs::path& host) {
    history_fixture::Directory profile;
    const std::string bin = domain::rebin_id;
    const auto toggle = UINT(win::WorkspaceCommand::rebin);
    domain::Id a, floating;
    { HistoryStore s(profile.path); save_shortcut(s.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"}); }
    auto check_rows = [](Desktop& app, const Order& expected) {
        require(until([&] {
            Order rows;
            for (const auto& row : app.frame().rows) rows.push_back(row.first);
            return rows == expected;
        }), "Unexpected ReBin row order");
    };
    {
        Desktop app(host, profile.path, true);
        for (UINT direction : {102u, 103u, toggle}) {
            action(app, toggle, false);
            check_rows(app, {bin});
            action(app, direction, false, {}, MenuExpectation{false, false, false, true, true, true});
            check_rows(app, {"main"});
        }
        action(app, toggle, false);
        auto properties = action(app, 106);
        require(text(properties, 1901) == L"ReBin" && text(properties, 1902) == L"Unlimited" &&
                (GetWindowLongPtrW(GetDlgItem(properties, 1901), GWL_STYLE) & ES_READONLY) &&
                (GetWindowLongPtrW(GetDlgItem(properties, 1902), GWL_STYLE) & ES_READONLY), "ReBin properties are editable");
        button(properties, IDCANCEL);
    }
    {
        HistoryStore s(profile.path);
        a = s.create_rope("A", 3);
        s.set_active_rope(a);
        floating = s.admit(a, sample('F'));
        s.detach(floating, {"", 40, 640, 220, 140});
    }
    {
        Desktop app(host, profile.path, true);
        Observe db(profile.path);
        auto tab = [&](WORD modifier = 0) {
            aim(app.workspace, center(app.frame().rows.front().second));
            if (modifier) key(modifier, true);
            key(VK_TAB, true); key(VK_TAB, false);
            if (modifier) key(modifier, false);
        };
        action(app, toggle, false);
        check_rows(app, {bin});
        tab(VK_SHIFT);
        check_rows(app, {bin});
        tab();
        check_rows(app, {a});
        action(app, toggle, false);
        check_rows(app, {bin});
        HWND card = app.floating_window();
        require(card && IsWindowVisible(card), "Missing independent Floating card");
        const auto dpi = GetDpiForWindow(card);
        POINT return_point{MulDiv(46, dpi, 96), MulDiv(19, dpi, 96)};
        ClientToScreen(card, &return_point);
        click(card, return_point);
        require(until([&] { return db.cards(bin) == Order{floating} && !IsWindow(card); }),
                "Floating return did not bind ReBin");
        const auto line = single_rope_point(app);
        aim(app.workspace, line);
        key(VK_CONTROL, true);
        INPUT wheel{};
        wheel.type = INPUT_MOUSE;
        wheel.mi.dwFlags = MOUSEEVENTF_WHEEL;
        wheel.mi.mouseData = 2 * WHEEL_DELTA;
        const auto sent = SendInput(1, &wheel, sizeof(wheel));
        key(VK_CONTROL, false);
        require(sent == 1, "Could not scroll from ReBin");
        check_rows(app, {a});
        action(app, UINT(win::WorkspaceCommand::overview), false);
        check_rows(app, {a, "main"});
        action(app, toggle, false, plaque(app, app.frame().rows.back().second));
        check_rows(app, {bin, a, "main"});
        auto rows = app.frame().rows;
        auto from = center(rows[0].second), to = center(rows[1].second);
        aim(app.workspace, from);
        mouse(MOUSEEVENTF_LEFTDOWN);
        require(SetCursorPos(to.x, to.y), "Could not drag ReBin");
        brief(80ms);
        mouse(MOUSEEVENTF_LEFTUP);
        check_rows(app, {a, bin, "main"});
        action(app, toggle, false, plaque(app, app.frame().rows[1].second));
        check_rows(app, {a, "main"});
        action(app, toggle, false, center(app.frame().rows[0].second));
        check_rows(app, {bin, a, "main"});
        tab();
        check_rows(app, {a, "main", bin});
        action(app, 102, false, center(app.frame().rows.front().second));
        check_rows(app, {bin, a, "main"});
        tab(VK_CONTROL);
        check_rows(app, {bin});
        action(app, 103, false);
        check_rows(app, {a});
        action(app, toggle, false);
        check_rows(app, {bin});
        SendMessageW(app.workspace, WM_CLOSE, 0, 0);
        require(!IsWindowVisible(app.workspace), "Workspace did not collapse");
        ClipboardPublisher writer;
        require(SUCCEEDED(writer.publish(::sample::formats(false))), "Synthetic hidden copy failed");
        require(until([&] { return db.cards(bin).size() == 2; }) && db.cards(a).empty(),
                "Hidden workspace lost its ReBin collector");
        key(VK_CONTROL,true); key(VK_MENU,true); key(VK_F11,true);
        key(VK_F11,false); key(VK_MENU,false); key(VK_CONTROL,false);
        require(until([&] { return IsWindowVisible(app.workspace) != 0; }), "Could not reopen the workspace");
        check_rows(app, {bin});
    }
    {
        Desktop app(host, profile.path, true);
        check_rows(app, {a});
        Observe db(profile.path);
        ClipboardPublisher writer;
        require(SUCCEEDED(writer.publish(::sample::formats(false))), "Synthetic restart copy failed");
        require(until([&] { return db.cards(a).size() == 1; }) && db.cards(bin).size() == 2,
                "Restart sent new history to ReBin");
    }
}
void immediate_drag(const fs::path& host) {
    history_fixture::Directory profile;
    domain::Id b, c;
    {
        HistoryStore store(profile.path);
        b = store.create_rope("B", 3);
        c = store.create_rope("C", 3);
        store.admit("main", sample('A'));
        store.admit(b, sample('B'));
        store.admit(c, sample('C'));
        store.set_active_rope("main");
        save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F11"});
    }
    // A white strip behind the empty left edge gives the screen samples a
    // known background throughout the physical gesture.
    MONITORINFO mon{sizeof(mon)};
    require(GetMonitorInfoW(MonitorFromWindow(GetForegroundWindow(), MONITOR_DEFAULTTOPRIMARY), &mon), "Missing monitor");
    struct Backdrop {
        HWND hwnd;
        ~Backdrop() { if (hwnd) DestroyWindow(hwnd); }
    } strip{CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", nullptr,
                            WS_POPUP | WS_VISIBLE | SS_WHITERECT | SS_NOTIFY, mon.rcWork.left, mon.rcWork.top, 48,
                            mon.rcWork.bottom - mon.rcWork.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr)};
    require(strip.hwnd != nullptr, "Could not create fixture backdrop");
    UpdateWindow(strip.hwnd);
    probe::Resident app(host.c_str(), profile.path, true);
    Observe db(profile.path);
    const HWND window = app.workspace;
    const auto dpi = GetDpiForWindow(window);
    const auto px = [&](int dip) { return MulDiv(dip, dpi, 96); };
    auto bounds = [&] { RECT r{}; GetWindowRect(window, &r); return r; };
    auto first_line = [&] {
        auto r = bounds();
        return POINT{r.left + px(2), r.top + px(int(presentation::Hanging::rope_end))};
    };
    struct Pointer {
        POINT start{};
        bool held = false;
        Pointer() { GetCursorPos(&start); }
        void release() { mouse(MOUSEEVENTF_LEFTUP); held = false; }
        ~Pointer() { if (held) mouse_event(MOUSEEVENTF_LEFTUP, 0, 0, 0, 0); SetCursorPos(start.x, start.y); }
    } pointer;
    auto press = [&](POINT p) {
        aim(window, p);
        const auto started = GetTickCount64();
        mouse(MOUSEEVENTF_LEFTDOWN);
        pointer.held = true;
        require(until([&] { return probe::owned_window(app.pid(), L"ClipRope.DragInput") != nullptr; }),
                "Rope line did not capture the physical press");
        return started;
    };
    const auto sequence = GetClipboardSequenceNumber();
    const auto initial = bounds();
    auto p = first_line();
    const auto began = press(p);
    require(SetCursorPos(p.x, p.y + 1), "Could not move the held pointer");
    const bool followed = until([&] { return bounds().top == initial.top + 1; }, 200ms);
    require(SetCursorPos(p.x, p.y + px(64)), "Could not reach the final height");
    const bool placed = until([&] { return bounds().top == initial.top + px(64); }, 350ms);
    pointer.release();
    const auto single_ms = GetTickCount64() - began;
    const bool saved = until([&] { return db.scalar("SELECT cast(round(top_dip) AS INTEGER) FROM rope_view_state WHERE rope_id='main'") == "88"; }, 500ms);

    SendMessageW(window, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::overview), 0);
    p = first_line();
    const auto overview_began = press(p);
    require(SetCursorPos(p.x, p.y + px(int(2 * presentation::Hanging::row))), "Could not reach the third row");
    pointer.release();
    const auto overview_ms = GetTickCount64() - overview_began;
    const bool inserted = until([&] {
        return db.scalar("SELECT group_concat(id, ',') FROM (SELECT id FROM ropes WHERE id<>'system:rebin' ORDER BY sort_rank,id)") == b + "," + c + ",main";
    }, 500ms);
    require(inserted, "Immediate overview drop did not insert at the third row");

    const auto pitch = px(int(presentation::Hanging::row));
    const auto shift = pitch / 4;
    require(until([&] { return WindowFromPoint({p.x, p.y + shift}) == strip.hwnd &&
                               WindowFromPoint({p.x, p.y + pitch - shift}) == strip.hwnd; }),
            "Fixture backdrop is covered");
    press(p);
    require(SetCursorPos(p.x, p.y + shift), "Could not move between row slots");
    const bool live = until([&] {
        auto dc = GetDC(nullptr);
        const auto rope_pixel = [&](LONG y) {
            const auto rgb = GetPixel(dc, p.x, y);
            return rgb != CLR_INVALID && GetBValue(rgb) < 208;
        };
        const bool moved = rope_pixel(p.y + shift) && rope_pixel(p.y + pitch - shift) &&
                           !rope_pixel(p.y) && !rope_pixel(p.y + pitch);
        ReleaseDC(nullptr, dc);
        return moved;
    });
    require(db.scalar("SELECT group_concat(id, ',') FROM (SELECT id FROM ropes WHERE id<>'system:rebin' ORDER BY sort_rank,id)") == b + "," + c + ",main",
            "Moving the preview committed while the button was down");
    key(VK_ESCAPE, true);
    key(VK_ESCAPE, false);
    pointer.release();
    app.exit();
    std::cout << "First pixel followed=" << followed << ", placed=" << placed << ", saved=" << saved
              << ", inserted at third row=" << inserted << ", single_ms=" << single_ms
              << ", overview_ms=" << overview_ms << ", double_click_ms=" << GetDoubleClickTime()
              << ", live rope and neighbor pixels=" << live << '\n';
    require(live, "Screen did not show the carried rope and its displaced neighbor before release");
    require(followed && placed && saved && inserted && single_ms < GetDoubleClickTime() &&
            overview_ms < GetDoubleClickTime(), "Rope drag did not follow the pointer immediately");
    require(db.active() == b && sequence != GetClipboardSequenceNumber() && clipboard_text_is(L"B"),
            "Rope drag did not copy the new collector's head");
}
void inline_shrink(const fs::path& exe) {
    history_fixture::Directory profile;
    {
        HistoryStore store(profile.path);
        store.set_capacity("main",5);
        for(char ch='A';ch<='E';++ch) store.admit("main",sample(ch));
    }
    Observe db(profile.path);
    const auto five=db.cards("main");
    const auto seq=GetClipboardSequenceNumber();
    auto review=[&](Desktop& app) {
        click(app.workspace,single_rope_point(app),true);
        HWND popup{};
        require(until([&]{popup=owned_window(pid(app.workspace),L"ClipRope.RopeMenu");return popup && IsWindowVisible(popup);}),
            "Inline rope menu unavailable");
        auto edit=GetDlgItem(popup,11902); RECT r{};
        require(edit && GetWindowRect(edit,&r),"Capacity field missing");
        click(edit,center(r)); set(popup,11902,L"3");
        key(VK_RETURN,true);key(VK_RETURN,false);
        HWND confirm{};
        require(until([&]{confirm=owned_window(pid(app.workspace),L"ClipRope.RopeManager");
            return confirm && text(confirm,1905).find(L"Remove 2 Hanging")!=std::wstring::npos;}),
            "Inline shrinking did not preview the exact tail");
        require(db.cards("main")==five,"Preview already removed a card");
        return confirm;
    };
    {
        Desktop app(exe,profile.path,true,L"rope-change");
        auto panel=review(app);button(panel,IDOK,false);
        require(until([&]{return text(panel,1905).find(L"could not be committed")!=std::wstring::npos;}),
            "Commit failure was not visible");
        require(db.cards("main")==five && db.scalar("SELECT capacity FROM ropes WHERE id='main'")=="5","Rollback lost history");
        button(panel,IDCANCEL);
    }
    {
        Desktop app(exe,profile.path,true);
        button(review(app),IDCANCEL);
        require(db.cards("main")==five,"Cancellation lost the suffix");
        button(review(app),IDOK);
        require(db.cards("main")==Order(five.begin(),five.begin()+3) &&
            db.scalar("SELECT capacity FROM ropes WHERE id='main'")=="3","Shrink committed a different subset");
    }
    require(seq==GetClipboardSequenceNumber(),"Inline property editing changed the clipboard");
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    const bool drag = argc == 3 && std::wstring_view(argv[2]) == L"--immediate-drag";
    const bool rebin = argc == 3 && std::wstring_view(argv[2]) == L"--rebin";
    if (argc != 2 && !drag && !rebin)
        return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr)))
        return 1;
    int result = 1;
    try {
        if (drag) immediate_drag(fs::absolute(argv[1]));
        else if (rebin) rebin_navigation(fs::absolute(argv[1]));
        else {
            run(fs::absolute(argv[1]));
            failed_creation_and_selection(fs::absolute(argv[1]));
            inline_shrink(fs::absolute(argv[1]));
        }
        result = 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
    }
    OleUninitialize();
    return result;
}
