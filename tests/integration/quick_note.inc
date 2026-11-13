void quick_note_keys(bool repeat = false) {
    key(VK_CONTROL, true); key(VK_SHIFT, true); key(VK_SPACE, true);
    if (repeat) {
        brief(180ms);
        key(VK_SPACE, true); key(VK_SPACE, true);
    }
    key(VK_SPACE, false); key(VK_SHIFT, false); key(VK_CONTROL, false);
}

void type_note_character(wchar_t ch) {
    INPUT chars[2]{};
    chars[0].type = chars[1].type = INPUT_KEYBOARD;
    chars[0].ki.wScan = chars[1].ki.wScan = ch;
    chars[0].ki.dwFlags = KEYEVENTF_UNICODE;
    chars[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    require(SendInput(2, chars, sizeof(INPUT)) == 2, "Synthetic Unicode input failed");
}

void quick_notes(const fs::path& exe, bool physical_ime) {
    require(!FindWindowW(L"ClipRope.Listener", nullptr), "Close other ClipRope hosts before testing the global shortcut");
    history_fixture::Directory profile;
    Order rope;
    {
        HistoryStore store(profile.path);
        store.set_capacity("main", 2);
        const auto tail = review_text(store);
        rope = {review_text(store), tail};
    }
    struct InputFixture {
        POINT cursor{};
        bool restore = GetCursorPos(&cursor) != FALSE;
        HWND field = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"EDIT", L"External fixture",
            WS_POPUP | WS_VISIBLE, 40, 100, 150, 40, nullptr, nullptr, nullptr, nullptr);
        ~InputFixture() { DestroyWindow(field); if (restore) SetCursorPos(cursor.x, cursor.y); }
    } input;
    require(input.field, "External field unavailable");
    RECT work{};
    require(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) &&
            SetWindowPos(input.field, HWND_TOPMOST, work.left + 40, work.bottom - 100, 150, 40, SWP_NOACTIVATE),
            "Could not place the external field below the test rope");
    const auto sequence = GetClipboardSequenceNumber();
    auto active_editor = [&](DWORD pid, HWND previous_card = nullptr) {
        HWND editor{};
        require(until([&] {
            editor = owned_window(pid, L"ClipRope.InlineEditorHost");
            GUITHREADINFO focus{sizeof(focus)};
            const HWND card = GetWindow(editor, GW_OWNER);
            return editor && card && card != previous_card && GetForegroundWindow() == editor &&
                   GetGUIThreadInfo(0, &focus) && focus.hwndFocus == GetDlgItem(editor, 1760);
        }), "Quick note did not receive native text focus");
        return editor;
    };

    // Occupy the global binding in another process. The actual menu button must still work.
    require(RegisterHotKey(input.field, 1, MOD_CONTROL | MOD_SHIFT, VK_SPACE), "Shortcut already reserved before fixture");
    {
        Desktop app(exe, profile.path, true);
        DWORD pid{};
        GetWindowThreadProcessId(app.workspace, &pid);
        HWND listener = owned_window(pid, L"ClipRope.Listener");
        PostMessageW(listener, WM_COMMAND, win::tray_command::settings, 0);
        HWND settings{};
        require(until([&] {
            settings = owned_window(pid, L"ClipRope.Settings");
            const auto status = GetDlgItem(FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr), 1806);
            return status && read(status) == L"Unavailable for Quick note";
        }), "Settings did not report the separate quick-note conflict");
        SendMessageW(settings, WM_CLOSE, 0, 0);
        press(input.field, center(bounds(input.field)));
        require(AllowSetForegroundWindow(pid), "Cannot hand input to the tray fixture");
        PostMessageW(listener, win::Tray::callback, MAKELPARAM(300,300), MAKELPARAM(WM_CONTEXTMENU,win::Tray::icon_id));
        HWND row{};
        require(until([&] {
            const auto menu = owned_window(pid, L"ClipRope.TrayMenu");
            return menu && (row = GetDlgItem(menu, win::tray_command::quick_note)) && IsWindowVisible(row);
        }), "Quick note menu button missing");
        press(row, center(bounds(row)));
        const auto editor = active_editor(pid);
        const auto field = GetDlgItem(editor, 1760);
        require_empty_caret(field);
        if (physical_ime) type_ime_candidate(editor, GetWindow(editor, GW_OWNER));
        else {
            type_note_character(L'记');
            require(until([&] { return read(field) == L"记"; }), "First typed character missed tray note");
            key(VK_BACK, true); key(VK_BACK, false);
            require_empty_caret(field);
            key(VK_TAB, true); key(VK_TAB, false);
            require(until([&] {
                GUITHREADINFO state{sizeof(state)};
                return GetGUIThreadInfo(0, &state) && state.hwndFocus == GetDlgItem(editor, IDOK);
            }), "Tab did not leave the empty note");
            key(VK_SHIFT, true); key(VK_TAB, true); key(VK_TAB, false); key(VK_SHIFT, false);
            require_empty_caret(field);
        }
        key(VK_ESCAPE, true); key(VK_ESCAPE, false);
        require(until([&] { return !IsWindowVisible(editor); }) && IsWindowVisible(app.floating_window()),
                "First cancel did not keep an empty Floating card");
        require(IsWindowVisible(app.workspace), "Tray note collapsed the open workspace");
        app.stop();
        require(app.clean_exit, "Could not close menu fixture");
    }
    UnregisterHotKey(input.field, 1);
    domain::Id blank;
    {
        HistoryStore store(profile.path);
        auto notes = store.floating();
        require(notes.size() == 1 && store.begin_text_edit(notes.front().id).text.empty(), "Canceled note was not blank");
        blank = notes.front().id;
    }
    {
        Desktop app(exe, profile.path);
        DWORD pid{};
        GetWindowThreadProcessId(app.workspace, &pid);
        press(input.field, center(bounds(input.field)));
        require(until([&] { return GetForegroundWindow() == input.field; }), "External test field did not take focus");
        quick_note_keys(true);
        const auto editor = active_editor(pid);
        require_empty_caret(GetDlgItem(editor, 1760));
        const HWND card = GetWindow(editor, GW_OWNER);
        MONITORINFO display{sizeof(display)};
        POINT cursor{};
        require(GetCursorPos(&cursor) && GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &display),
                "Could not read pointer monitor");
        const auto rect = bounds(card);
        const auto mid = center(rect), expected = center(display.rcWork);
        const UINT dpi = GetDpiForWindow(card);
        require(std::abs(mid.x - expected.x) <= 1 && std::abs(mid.y - expected.y) <= 1 &&
                rect.right-rect.left == MulDiv(240,dpi,96) && rect.bottom-rect.top == MulDiv(160,dpi,96),
                "Quick note is not centered at its text-card size");
        type_note_character(L'记');
        key(VK_RETURN, true); key(VK_RETURN, false);
        type_note_character(L'记');
        require(until([&] { return read(GetDlgItem(editor, 1760)) == L"记\r\n记"; }), "Unicode text or newline lost");

        sqlite3* raw{};
        const auto path = (profile.path / "history.db").u8string();
        require(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
                "Cannot inject edit failure");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        const auto field = GetDlgItem(editor, 1760);
        const std::wstring wrapped(100, L'W');
        SendMessageW(field, WM_SETTEXT, 0, LPARAM(wrapped.c_str()));
        SendMessageW(field, EM_SETSEL, 4, 9);
        const auto lines = SendMessageW(field, EM_GETLINECOUNT, 0, 0);
        PostMessageW(owned_window(pid, L"ClipRope.Listener"), WM_COMMAND, win::tray_command::settings, 0);
        HWND settings{};
        require(until([&] { settings = owned_window(pid, L"ClipRope.Settings"); return settings && IsWindowVisible(settings); }),
                "Could not open Settings with a live draft");
        SendDlgItemMessageW(settings, 1501, BM_CLICK, 0, 0);
        auto font = GetDlgItem(FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr), 1921);
        SendMessageW(font, WM_SETTEXT, 0, LPARAM(L"40"));
        PostMessageW(font, WM_KEYDOWN, VK_RETURN, 0);
        require(until([&] { return SendMessageW(field, EM_GETLINECOUNT, 0, 0) > lines; }) &&
                read(field) == wrapped && SendMessageW(field, EM_GETSEL, 0, 0) == MAKELONG(4, 9),
                "Live font update failed to reflow the draft and preserve its selection");
        SendMessageW(settings, WM_CLOSE, 0, 0);
        SendMessageW(field, WM_SETTEXT, 0, LPARAM(L"记\r\n记"));
        exec(raw, "CREATE TRIGGER reject_note BEFORE UPDATE OF snapshot_id ON entries BEGIN SELECT RAISE(ABORT,'test edit'); END");
        quick_note_keys();
        require(until([&] { return read(GetDlgItem(editor, 1761)).find(L"failed") != std::wstring::npos; }) &&
                active_editor(pid) == editor && GetWindow(editor, GW_OWNER) == card &&
                read(GetDlgItem(editor, 1760)) == L"记\r\n记", "Failed handoff lost the original draft");
        exec(raw, "DROP TRIGGER reject_note");
        // Two separate presses arrive while the old draft is waiting for its writer.
        exec(raw, "BEGIN IMMEDIATE");
        quick_note_keys();
        require(until([&] { return !IsWindowVisible(editor); }, 700ms), "Original save was not pending");
        quick_note_keys();
        exec(raw, "COMMIT");
        require(until([&] {
            Statement count(raw, "SELECT count(*) FROM entries WHERE state='Floating'");
            return count.step() && count.integer(0) == 4;
        }), "Distinct presses during a pending save were lost");
        active_editor(pid, card);
        require_empty_caret(GetDlgItem(editor, 1760));
        type_note_character(L'存');
        key(VK_CONTROL, true); key('S', true); key('S', false); key(VK_CONTROL, false);
        require(until([&] { return !IsWindowVisible(editor); }), "Ctrl+S did not return to preview");
        require(!IsWindowVisible(app.workspace) && GetClipboardSequenceNumber() == sequence,
                "Quick note toggled the workspace or published clipboard data");
        app.stop();
        require(app.clean_exit, "Could not close shortcut fixture");
    }
    HistoryStore saved_notes(profile.path);
    const auto notes = saved_notes.floating();
    require(notes.size() == 4 && saved_notes.begin_text_edit(blank).text.empty() &&
            std::count_if(notes.begin(), notes.end(), [&](const auto& e) {
                return saved_notes.begin_text_edit(e.id).text == L"记\r\n记";
            }) == 1 && std::count_if(notes.begin(), notes.end(), [&](const auto& e) {
                return saved_notes.begin_text_edit(e.id).text == L"存";
            }) == 1 && order(profile.path) == rope, "Unexpected note count, saved text or full-rope order");
}
