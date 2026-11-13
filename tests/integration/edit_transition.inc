// Observe composed desktop pixels independently of the application's paint callbacks.
struct InsetCapture {
    RECT area;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    void* bits{};
    HBITMAP bitmap{};
    HGDIOBJ previous{};
    explicit InsetCapture(RECT r) : area(r) {
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), r.right-r.left, r.top-r.bottom, 1, 32};
        bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        previous = SelectObject(dc, bitmap);
        require(previous && bits, "Could not sample the composed editor inset");
    }
    ~InsetCapture() {
        SelectObject(dc, previous); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr, screen);
    }
    std::vector<DWORD> sample() {
        const auto w = area.right-area.left, h = area.bottom-area.top;
        require(BitBlt(dc, 0, 0, w, h, screen, area.left, area.top, SRCCOPY | CAPTUREBLT), "Screen sample failed");
        GdiFlush();
        const auto pixels = static_cast<DWORD*>(bits);
        return {pixels, pixels + w*h};
    }
};

void require_empty_caret(HWND field) {
    GUITHREADINFO state{sizeof(state)};
    RECT client{};
    require(until([&] {
        return GetGUIThreadInfo(GetWindowThreadProcessId(field, nullptr), &state) &&
               state.hwndFocus == field && state.hwndCaret == field && GetClientRect(field, &client) &&
               SendMessageW(field, WM_GETTEXTLENGTH, 0, 0) == 0 && !IsRectEmpty(&state.rcCaret) &&
               state.rcCaret.left == MulDiv(2, GetDpiForWindow(field), 96) && state.rcCaret.top >= 0 &&
               state.rcCaret.right <= client.right && state.rcCaret.bottom <= client.bottom;
    }), "Empty Rich Edit caret did not occupy the first line inside the paper frame");

    // Compare the middle of the caret with the blank paper just to its right.
    POINT point{state.rcCaret.left, (state.rcCaret.top + state.rcCaret.bottom) / 2};
    require(ClientToScreen(field, &point), "Could not locate the caret on screen");
    const int width = state.rcCaret.right - state.rcCaret.left;
    InsetCapture capture({point.x, point.y, point.x + width + 3, point.y + 1});
    const auto blink = GetCaretBlinkTime();
    const bool steady = blink == INFINITE || blink == 0;
    bool ink = false, paper = false;
    require(until([&] {
        if (WindowFromPoint(point) != field) return false;
        const auto pixels = capture.sample();
        const auto a = pixels.front(), b = pixels.back();
        const int contrast = std::max({std::abs(GetRValue(a) - GetRValue(b)),
                                       std::abs(GetGValue(a) - GetGValue(b)),
                                       std::abs(GetBValue(a) - GetBValue(b))});
        ink |= contrast >= 96;
        paper |= contrast < 32;
        return ink && (steady || paper);
    }, steady ? 1500ms : 2 * std::chrono::milliseconds(blink) + 750ms),
        "The empty caret was not visible on the composed paper at the system blink interval");
}

void interrupted_edit(const fs::path& exe, bool evict) {
    history_fixture::Directory profile;
    domain::Id id, neighbor;
    {
        HistoryStore store(profile.path);
        neighbor = review_text(store);
        id = review_text(store);
    }
    Desktop app(exe, profile.path, true);
    auto host = select_command(app.workspace, center(app.card(id)), Command::edit);
    auto field = GetDlgItem(host, 1760);
    std::wstring lines;
    for (int i = 0; i < 20; ++i) lines += L"Multiline draft\r\n";
    set(host, 1760, lines.c_str());
    SendMessageW(field, EM_SETSEL, 0, 5);
    SendMessageW(field, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"Changed"));
    SendMessageW(field, EM_SETSEL, 60, 67);
    SendMessageW(field, EM_LINESCROLL, 0, 7);
    const auto original = read(field);
    const auto first_line = SendMessageW(field, EM_GETFIRSTVISIBLELINE, 0, 0);
    const auto selection = SendMessageW(field, EM_GETSEL, 0, 0);
    const auto clipboard = GetClipboardSequenceNumber();
    sqlite3* raw{};
    auto path = (profile.path / "history.db").u8string();
    require(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Could not open the interruption fixture");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    exec(raw, "PRAGMA foreign_keys=ON");
    exec(raw, "BEGIN IMMEDIATE");
    PostMessageW(host, WM_COMMAND, IDOK, 0);
    require(until([&] { return !IsWindowVisible(host); }, 400ms), "Delayed save did not present its draft");
    PostMessageW(app.workspace, WM_CLOSE, 0, 0); // Collapse is contingent on saving.
    if (evict) {
        Statement remove(raw, "DELETE FROM entries WHERE id=?");
        remove.bind(1, id);
        remove.run();
    } else exec(raw, "CREATE TRIGGER reject_text BEFORE UPDATE OF snapshot_id ON entries "
                     "BEGIN SELECT RAISE(ABORT,'rejected edit'); END");
    exec(raw, "COMMIT");
    if (evict) {
        app.check_display({neighbor});
        require(!IsWindowVisible(host) && !GetWindow(host, GW_OWNER), "Eviction rebound the draft to a neighbor");
    } else {
        require(until([&] { return IsWindowVisible(host) && read(GetDlgItem(host, 1761)).find(L"failed") != std::wstring::npos; }),
                "Rejected pending edit did not restore its native field");
        require(IsWindowVisible(app.workspace) && read(field) == original, "Rollback lost the draft or collapsed the workspace");
        require(SendMessageW(field, EM_CANUNDO, 0, 0), "Rollback cleared the undo stack");
        require(SendMessageW(field, EM_GETSEL, 0, 0) == selection, "Rollback changed the selected characters");
        require(SendMessageW(field, EM_GETFIRSTVISIBLELINE, 0, 0) == first_line, "Rollback changed the first visible line");
        require(GetClipboardSequenceNumber() == clipboard, "Rejected edit published its draft");
        exec(raw, "DROP TRIGGER reject_text");
        saved(host);
    }
    require(IsWindowVisible(app.workspace) &&
            (evict ? GetClipboardSequenceNumber() == clipboard : until([&] { return clipboard_text_is(original); })),
            "Rejected edit collapsed the workspace or retry did not publish the saved head");
    app.stop();
    db.reset();
    HistoryStore reopened(profile.path);
    require(app.clean_exit && reopened.entry(neighbor).revision == 1, "A late completion changed another card");
    if (!evict) require(reopened.entry(id).revision == 2 && reopened.begin_text_edit(id).text == original,
                        "Retry did not install exactly one edit");
}

void recall_submitted_preview(const fs::path& exe, bool commit_between_clicks) {
    history_fixture::Directory profile;
    domain::Id id;
    { HistoryStore store(profile.path); id = review_text(store); }
    Desktop app(exe, profile.path, true);
    auto pt = center(app.card(id));
    auto host = select_command(app.workspace, pt, Command::edit);
    const wchar_t text[] = L"Recall the submitted text";
    set(host, 1760, text);
    auto file = (profile.path / "history.db").u8string();
    sqlite3* raw{};
    require(sqlite3_open_v2(reinterpret_cast<const char*>(file.c_str()), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
            "Could not delay recall");
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    exec(raw, "BEGIN IMMEDIATE");
    SendMessageW(host, WM_COMMAND, IDOK, 0);
    require(!IsWindowVisible(host), "Submitted preview was unavailable to recall");
    const auto before = GetClipboardSequenceNumber();
    ScreenToClient(app.workspace, &pt);
    const auto where = MAKELPARAM(pt.x, pt.y);
    SendMessageW(app.workspace, WM_LBUTTONDOWN, MK_LBUTTON, where);
    SendMessageW(app.workspace, WM_LBUTTONUP, 0, where);
    if (commit_between_clicks) {
        exec(raw, "ROLLBACK");
        require(until([&] { return !GetWindow(host, GW_OWNER); }), "Commit did not finish between clicks");
    }
    SendMessageW(app.workspace, WM_LBUTTONDBLCLK, MK_LBUTTON, where);
    SendMessageW(app.workspace, WM_LBUTTONUP, 0, where);
    if (!commit_between_clicks) {
        require(IsWindowVisible(app.workspace) && before == GetClipboardSequenceNumber(), "Recall published before commit");
        exec(raw, "ROLLBACK");
    }
    require(until([&] { return !IsWindowVisible(app.workspace) && before != GetClipboardSequenceNumber(); }),
            "Submitted preview lost its bound recall");
    require(until([] { return !!OpenClipboard(nullptr); }), "Could not inspect recalled text");
    const auto data = GetClipboardData(CF_UNICODETEXT);
    const auto value = static_cast<const wchar_t*>(data ? GlobalLock(data) : nullptr);
    const bool matches = value && std::wstring_view(value) == text;
    if (value) GlobalUnlock(data);
    CloseClipboard();
    require(matches, "Recall published the preceding snapshot");
}

void edit_transition(const fs::path& exe, bool baseline) {
    using Clock = std::chrono::steady_clock;
    for (const std::string mode : {"single", "overview", "floating", "floating-opaque", "transparent"}) {
        history_fixture::Directory profile;
        domain::Id id;
        const bool floating = mode.starts_with("floating");
        const double opacity = mode == "transparent" ? 0. : mode == "overview" || mode == "floating" ? .65 : 1.;
        {
            HistoryStore store(profile.path);
            id = review_text(store);
            store.save_style(id, {16, true, 0x101010, 0xffecce, opacity});
            if (floating) store.detach(id, {"", 640, 330, 240, 160});
        }
        PaperBackdrop backdrop;
        Desktop app(exe, profile.path, true);
        if (mode == "overview") SendMessageW(app.workspace, WM_COMMAND, UINT(win::WorkspaceCommand::overview), 0);
        const auto owner = floating ? app.floating_window() : app.workspace;
        const auto paper = floating ? bounds(owner) : app.card(id);
        require(SetWindowPos(backdrop.window, owner, paper.left, paper.top, paper.right-paper.left,
                paper.bottom-paper.top, SWP_NOACTIVATE | SWP_SHOWWINDOW), "Missing contrast backdrop");
        const auto file = (profile.path / "history.db").u8string();
        sqlite3* raw{};
        require(sqlite3_open_v2(reinterpret_cast<const char*>(file.c_str()), &raw, SQLITE_OPEN_READWRITE, nullptr) == SQLITE_OK,
                "Could not delay the test store");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
        std::vector<DWORD> preceding;
        const auto clipboard = GetClipboardSequenceNumber();
        for (int trial = 0; trial < 2; ++trial) {
            HWND editor{};
            if (opacity != 0) editor = select_command(owner, center(paper), Command::edit);
            else {
                const auto dpi = GetDpiForWindow(owner);
                press(owner, {paper.right - MulDiv(12, dpi, 96), paper.top - MulDiv(9, dpi, 96)});
                DWORD pid{};
                GetWindowThreadProcessId(owner, &pid);
                require(until([&] { editor = owned_window(pid, L"ClipRope.InlineEditorHost");
                    return editor && IsWindowVisible(editor); }), "Transparent paper did not open");
            }
            set(editor, 1760, trial ? L"Repeat\r\n\u9884\u89c8" : L"First\r\n\u9884\u89c8");
            auto area = bounds(editor);
            InflateRect(&area, -2, -2);
            // Zero-opacity paper intentionally retains its upper-right close control.
            if (opacity == 0) area.top += (area.bottom - area.top) / 2;
            InsetCapture capture(area);
            exec(raw, "BEGIN IMMEDIATE");
            const auto start = Clock::now();
            require(PostMessageW(editor, WM_COMMAND, IDOK, 0), "Could not submit save");
            PostMessageW(editor, WM_COMMAND, IDOK, 0); // A second click must not commit twice.
            std::vector<std::pair<double, std::vector<DWORD>>> samples;
            bool responsive = false, released = false;
            while (Clock::now()-start < 850ms) {
                const auto now = Clock::now()-start;
                if (!released && now >= 500ms) { exec(raw, "ROLLBACK"); released = true; }
                if (!IsWindowVisible(editor) && !released) {
                    DWORD_PTR result{};
                    responsive |= !!SendMessageTimeoutW(owner, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 25, &result);
                }
                auto frame = capture.sample();
                samples.emplace_back(std::chrono::duration<double, std::milli>(Clock::now()-start).count(), std::move(frame));
                Sleep(8);
            }
            require(released && !IsWindowVisible(editor), "Delayed edit did not finish");
            const auto expected = capture.sample();
            const auto first = std::find_if(samples.begin(), samples.end(), [&](const auto& s) { return s.second == expected; });
            require(first != samples.end(), "Could not observe a stable preview frame");
            std::cout << "transition=" << mode << " trial=" << trial+1 << " first_preview_ms=" << first->first
                      << " responsive_before_commit=" << responsive << std::endl;
            if (!baseline) {
                require(first->first < 500 && responsive, "Preview or message loop waited for the writer");
                if (opacity > 0) {
                    for (const auto& s : samples)
                        require(std::adjacent_find(s.second.begin(), s.second.end(), std::not_equal_to<>()) != s.second.end(),
                                "Editor inset briefly exposed the uniform backdrop");
                    require(expected != preceding, "Changed paragraphs retained the previous preview");
                } else require(std::adjacent_find(expected.begin(), expected.end(), std::not_equal_to<>()) == expected.end(),
                               "Zero opacity did not restore the backdrop");
                preceding = expected;
            }
        }
        app.stop();
        db.reset();
        HistoryStore reopened(profile.path);
        require(app.clean_exit && reopened.entry(id).revision == 3 &&
                reopened.begin_text_edit(id).text == L"Repeat\r\n\u9884\u89c8" &&
                (floating ? clipboard == GetClipboardSequenceNumber() : clipboard_text_is(L"Repeat\r\n\u9884\u89c8")),
                "Repeated saves lost the expected clipboard, revision or durable text");
    }
    if (!baseline) {
        interrupted_edit(exe, false);
        interrupted_edit(exe, true);
        recall_submitted_preview(exe, false);
        recall_submitted_preview(exe, true);
    }
}
