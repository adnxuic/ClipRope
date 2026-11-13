// Optional review on an unlocked test desktop. Capture just the synthetic card
// against a test-owned neutral surface, including the actual HWND composition.
struct PaperBackdrop {
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"STATIC", L"",
        WS_POPUP | SS_GRAYRECT, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~PaperBackdrop() { if (window) DestroyWindow(window); }
    void capture(HWND owner, RECT r, const fs::path& path) {
        const LONG w = r.right - r.left, h = r.bottom - r.top;
        require(window && SetWindowPos(window, owner, r.left, r.top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW),
                "Could not position the review backdrop");
        UpdateWindow(window);
        brief(120ms);
        HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
        BITMAPINFO info{};
        info.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32};
        void* pixels{};
        auto image = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
        require(dc && pixels, "Review image unavailable");
        auto old = SelectObject(dc, image);
        const bool copied = BitBlt(dc, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT) != FALSE;
        GdiFlush();
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(info.bmiHeader);
        header.bfSize = header.bfOffBits + 4 * w * h;
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        out.write(static_cast<const char*>(pixels), 4 * w * h);
        SelectObject(dc, old);
        DeleteObject(image); DeleteDC(dc); ReleaseDC(nullptr, screen);
        require(copied && out.good(), "Could not record review image");
    }
};

domain::Id review_text(HistoryStore& store) {
    domain::Format format;
    format.standard_id = CF_UNICODETEXT;
    const wchar_t sample[] = L"Meeting notes\r\n\u6574\u7406\u4f1a\u8bae\u7b14\u8bb0\r\nCheck the next draft.";
    const auto* data = reinterpret_cast<const unsigned char*>(sample);
    format.bytes.assign(data, data + sizeof(sample));
    return store.admit("main", {{format}});
}

void paper_gallery(const fs::path& exe, const fs::path& directory) {
    fs::create_directories(directory);
    history_fixture::Directory profile;
    domain::Id id;
    { HistoryStore store(profile.path); id = review_text(store); }
    for (std::string mode : {"single", "overview", "compact", "transparent", "floating"}) {
        {
            HistoryStore store(profile.path);
            store.save_style(id, {14, true, 0x21443c, 0xfff3d5, mode == "transparent" ? 0. : mode == "overview" ? .65 : 1.});
            if (mode == "floating") store.detach(id, {"", 650, 360, 240, 160});
        }
        PaperBackdrop backdrop;
        Desktop app(exe, profile.path, true);
        if (mode == "overview") SendMessageW(app.workspace, WM_COMMAND, UINT(win::WorkspaceCommand::overview), 0);
        if (mode == "compact") SendMessageW(app.workspace, WM_COMMAND, UINT(win::WorkspaceCommand::density), 0);
        const auto floating = mode == "floating";
        const auto owner = floating ? app.floating_window() : app.workspace;
        const auto paper = floating ? bounds(owner) : app.card(id);
        const auto dpi = GetDpiForWindow(owner);
        auto px = [&](int dip) { return MulDiv(dip, dpi, 96); };
        RECT capture = paper;
        if (!floating) { InflateRect(&capture, px(9), px(8)); capture.top -= px(48); }
        POINT pencil = floating ? POINT{paper.right - px(45), paper.top + px(18)} :
                                  POINT{paper.right - px(12), paper.top - px(9)};
        SetCursorPos(paper.right + px(50), paper.bottom + px(40));
        brief(1200ms);
        backdrop.capture(owner, capture, directory / (mode + "-preview.bmp"));
        press(owner, pencil);
        DWORD pid{}; GetWindowThreadProcessId(owner, &pid);
        HWND host{};
        require(until([&] {
            host = owned_window(pid, L"ClipRope.InlineEditorHost");
            return host && IsWindowVisible(host);
        }), "Gallery pencil did not open the editor");
        auto field = GetDlgItem(host, 1760);
        SendMessageW(field, EM_SETSEL, 0, 7);
        backdrop.capture(owner, capture, directory / (mode + "-edit.bmp"));
        std::cout << "paper_gallery=" << mode << " dpi=" << dpi << '\n';
        SendMessageW(host, WM_COMMAND, IDCANCEL, 0);
        app.stop();
    }
}

void type_ime_candidate(HWND host, HWND paper, const fs::path& image = {}) {
    const auto field = GetDlgItem(host, 1760);
    // Physical letter keys go through the installed Chinese input method.
    // A plain "nihao " is not evidence of composition or candidate selection.
    bool selected = false;
    for (int pass = 0; pass < 2 && !selected; ++pass) {
        set(host, 1760, L"");
        if (pass) { key(VK_SHIFT, true); key(VK_SHIFT, false); brief(200ms); }
        GUITHREADINFO before{sizeof before}, after{sizeof after};
        GetGUIThreadInfo(GetWindowThreadProcessId(host, nullptr), &before);
        for (const auto ch : std::wstring_view(L"NIHAO")) { key(ch, true); key(ch, false); brief(60ms); }
        brief(350ms);
        GetGUIThreadInfo(GetWindowThreadProcessId(host, nullptr), &after);
        RECT caret = after.rcCaret;
        MapWindowPoints(after.hwndCaret, nullptr, reinterpret_cast<POINT*>(&caret), 2);
        std::cout << "caret_rect=" << caret.left << ',' << caret.top << ',' << caret.right << ',' << caret.bottom << '\n';
        if (!image.empty()) {
            const auto dpi = GetDpiForWindow(host);
            RECT around{std::max(0L, caret.left - MulDiv(50, dpi, 96)),
                        std::max(0L, caret.top - MulDiv(40, dpi, 96)),
                        caret.left + MulDiv(430, dpi, 96), caret.bottom + MulDiv(90, dpi, 96)};
            PaperBackdrop backdrop;
            backdrop.capture(paper, around, image);
        }
        require(before.hwndFocus == field && after.hwndFocus == field, "Composition lost native focus");
        key(VK_SPACE, true); key(VK_SPACE, false);
        selected = until([&] { return read(field) == L"\u4f60\u597d"; }, 900ms);
    }
    require(selected, "Installed IME did not commit the expected Chinese candidate");
}

void ime_review(const fs::path& exe, const fs::path& image) {
    history_fixture::Directory profile;
    domain::Id id;
    { HistoryStore store(profile.path); id = review_text(store); }
    Desktop app(exe, profile.path, true);
    auto host = select_command(app.workspace, center(app.card(id)), Command::edit);
    type_ime_candidate(host, app.workspace, image);
    key(VK_RETURN, true); key(VK_RETURN, false);
    key(VK_CONTROL, true); key('S', true); key('S', false); key(VK_CONTROL, false);
    require(until([&] { return !IsWindowVisible(host); }), "IME text did not save");
    host = select_command(app.workspace, center(app.card(id)), Command::edit);
    require(read(GetDlgItem(host, 1760)) == L"\u4f60\u597d\r\n", "Reopened IME text differs");
    SendMessageW(GetDlgItem(host, 1760), EM_SETSEL, 4, 4);
    for (auto ch : std::wstring_view(L"NIHAO")) { key(ch, true); key(ch, false); brief(60ms); }
    brief(250ms);
    SendMessageW(app.workspace, WM_CLOSE, 0, 0);
    require(until([&] { return !IsWindowVisible(host) && !IsWindowVisible(app.workspace); }),
            "Collapse did not complete the pending composition");
    app.stop();
    HistoryStore store(profile.path);
    const auto saved = store.begin_text_edit(id).text;
    require(saved.starts_with(L"\u4f60\u597d\r\n") && saved.size() > 4,
            "Collapse dropped the active composition");
    std::cout << "physical_pinyin=nihao chinese_candidate_committed=1 multiline_restart=1 composition_autosave=1\n";
}
