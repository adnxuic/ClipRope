// Included beside Host so physical input, process lifetime and metric queries use the normal benchmark driver.
void benchmark_menus(const fs::path& exe, const fs::path& folder, bool quick, const std::string& only,
                     localization::Language language) {
    fs::create_directories(folder);
    std::ofstream csv(folder / "menus.csv");
    std::ofstream readiness(folder / "readiness.csv");
    require(bool(csv), "Cannot create menu timing output");
    require(bool(readiness), "Cannot create startup resource output");
    csv << "entry,open,trial,dispatch_ms,focus_ms,model_ms,controls_ms,frame_ms,paint_ms,compose_ms,total_ms\n";
    readiness << "language,entry,trial,ready_ms,host_private_bytes,worker_private_bytes,processes,gdi,user\n";
    LARGE_INTEGER hz{};
    QueryPerformanceFrequency(&hz);
    history_fixture::Directory dir;
    const auto profile = dir.path / "menus";
    {
        HistoryStore store(profile);
        save_language(store.database(), language);
        save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Alt+F24"});
        store.create_rope("Second", 48);
        auto floating = store.admit("main", snapshot(sample::formats(false)));
        store.detach(floating, {"", 400, 380, 240, 160});
        store.admit("main", snapshot(sample::formats(false)));
    }
    POINT original{};
    GetCursorPos(&original);
    struct RestoreCursor { POINT point; ~RestoreCursor() { SetCursorPos(point.x, point.y); } } restore{original};
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HWND receiver = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"Menu benchmark editor", WS_POPUP | WS_VISIBLE,
                                    work.right - 150, work.bottom - 44, 140, 40, nullptr, nullptr, nullptr, nullptr);
    struct DestroyReceiver { HWND window; ~DestroyReceiver() { if (window) DestroyWindow(window); } } receiver_guard{receiver};
    HWND field = CreateWindowExW(0, L"EDIT", L"Synthetic text", WS_CHILD | WS_VISIBLE, 2, 2, 136, 36,
                                 receiver, nullptr, nullptr, nullptr);
    require(receiver && field, "Synthetic foreground editor unavailable");
    bool ran = false;
    for (const std::string entry : {"single-card", "single-rope", "overview-card", "overview-rope", "floating",
                                    "large-rebin", "legacy-path"}) {
        if (!only.empty() && only != entry) continue;
        ran = true;
        const bool rope = entry.ends_with("rope"), floating = entry == "floating";
        if (entry == "large-rebin") {
            HistoryStore store(profile);
            const auto seed = store.hanging("main").front();
            Statement many(store.database().handle(),
                "WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<4096) "
                "INSERT INTO entries(id,state,snapshot_id,rope_id,sort_key) "
                "SELECT 'menu-seed-'||x,'Hanging',?,'system:rebin',x FROM n");
            many.bind(1, seed.snapshot_id);
            many.run();
        } else if (entry == "legacy-path") {
            HistoryStore store(profile);
            store.set_active_rope("main");
            // A legacy remote path has no persisted file/directory classification;
            // the preview worker requires an explicit request before checking it.
            store.admit("main", snapshot({image_file_sample::drop({fs::path(LR"(\\menu-fixture.invalid\share\item)")})}));
        }
        for (int cold = 0; cold < (quick ? 1 : 30); ++cold) {
            Host host(exe, profile);
            const auto usage = process_tree(host.pid);
            readiness << localization::language_code(language) << ',' << entry << ',' << cold+1 << ',' << host.startup << ','
                      << usage.host_private << ',' << usage.worker_private << ',' << usage.processes << ','
                      << GetGuiResources(host.process, GR_GDIOBJECTS) << ',' << GetGuiResources(host.process, GR_USEROBJECTS)
                      << std::endl;
            if (!floating) {
                send(host.listener, WM_APP + 150, 0, MAKELPARAM(NIN_SELECT, 1));
                require(wait([&] { return host.visible(); }), "Workspace did not expand");
                if (entry == "large-rebin")
                    send(host.workspace, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::rebin));
                if (entry.starts_with("overview"))
                    send(host.workspace, WM_COMMAND, static_cast<WPARAM>(win::WorkspaceCommand::overview));
            }
            HWND source = floating ? owned(host.pid, L"ClipRope.Floating") : host.workspace;
            require(source && IsWindowVisible(source), "Menu source unavailable");
            POINT at{};
            if (floating) {
                RECT r{}; GetWindowRect(source, &r);
                at = {(r.left+r.right)/2, (r.top+r.bottom)/2};
            } else {
                const auto point = host.metric(rope ? win::PerformanceCounter::first_rope_point
                                                    : win::PerformanceCounter::first_card_point);
                require(point, "No visible menu target");
                at = {GET_X_LPARAM(point), GET_Y_LPARAM(point)};
            }
            const DWORD sequence = GetClipboardSequenceNumber();
            for (int repeat = 0; repeat <= (cold == 0 ? (quick ? 5 : 100) : 0); ++repeat) {
                if (GetForegroundWindow() != receiver) {
                    SetForegroundWindow(receiver);
                    if (GetForegroundWindow() != receiver) {
                        POINT activate{work.right - 80, work.bottom - 24};
                        require(GetAncestor(WindowFromPoint(activate), GA_ROOT) == receiver,
                                "Synthetic foreground editor is covered");
                        SetCursorPos(activate.x, activate.y);
                        INPUT click[2]{};
                        click[0].type = click[1].type = INPUT_MOUSE;
                        click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
                        click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
                        require(SendInput(2, click, sizeof(INPUT)) == 2, "Editor activation failed");
                    }
                }
                require(wait([&] { return GetForegroundWindow() == receiver; }), "External editor lost foreground");
                SetFocus(field);
                // Let the previous window destruction and cursor move settle before the measured release.
                SetCursorPos(at.x, at.y);
                Sleep(40);
                require(WindowFromPoint(at) == source, "Menu target occluded; physical input canceled");
                INPUT mouse{}; mouse.type = INPUT_MOUSE;
                mouse.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN;
                require(SendInput(1, &mouse, sizeof(mouse)) == 1, "Right press failed");
                Sleep(30);
                LARGE_INTEGER release{}; QueryPerformanceCounter(&release);
                mouse.mi.dwFlags = MOUSEEVENTF_RIGHTUP;
                require(SendInput(1, &mouse, sizeof(mouse)) == 1, "Right release failed");
                HWND popup{};
                LRESULT composed{};
                require(wait([&] {
                    popup = owned(host.pid, rope ? L"ClipRope.RopeMenu" : L"ClipRope.CardMenu");
                    return popup && IsWindowVisible(popup) &&
                        (composed = send(popup, win::menu_timing_query, static_cast<WPARAM>(win::MenuStage::count))) > 0;
                }), "Menu composition receipt missing");
                if (entry == "large-rebin") {
                    wchar_t caption[128]{};
                    GetWindowTextW(popup, caption, 128);
                    require(std::wstring_view(caption).find(L"4096") != std::wstring_view::npos,
                            "Benchmark missed the populated ReBin");
                }
                if (entry == "legacy-path")
                    require(!GetDlgItem(popup, 1808) && GetDlgItem(popup, 1809) && GetDlgItem(popup, 1810),
                            "Unclassified remote file exposed a type-specific command");
                csv << entry << ',' << (repeat ? "warm" : "cold") << ',' << (repeat ? repeat : cold+1);
                auto previous = release.QuadPart;
                for (size_t i = 0; i < static_cast<size_t>(win::MenuStage::count); ++i) {
                    const auto t = send(popup, win::menu_timing_query, i);
                    require(t >= previous, "Missing or out-of-order menu timestamp");
                    csv << ',' << (t-previous)*1000./hz.QuadPart;
                    previous = t;
                }
                csv << ',' << (composed-previous)*1000./hz.QuadPart << ','
                    << (composed-release.QuadPart)*1000./hz.QuadPart << std::endl;
                PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
                require(wait([&] { return !IsWindow(popup) && !owned(host.pid, L"ClipRope.CardMenuFrame"); }), "Menu did not respond to Escape");
                require(sequence == GetClipboardSequenceNumber(), "Clipboard changed during menu benchmark");
            }
        }
    }
    require(ran, "Unknown menu benchmark entry");
}
