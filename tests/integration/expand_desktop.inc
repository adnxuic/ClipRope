void expand_desktop(const fs::path& exe, const fs::path& review) {
    history_fixture::Directory root;
    const auto first = root.path / "first.txt", second = root.path / "second.txt";
    std::ofstream(first).put('1');
    std::ofstream(second).put('2');
    domain::Format drop;
    drop.standard_id = CF_HDROP;
    drop.bytes = image_file_sample::drop({first, second}).bytes;
    domain::Snapshot files{{drop}};
    const auto sequence = GetClipboardSequenceNumber();
    for (const bool overview : {false, true}) {
        const auto profile = root.path / (overview ? "overview" : "single");
        domain::Id rope = "main", group, tail, middle;
        {
            HistoryStore store(profile);
            if (overview) rope = store.create_rope("Second", 4);
            else store.set_capacity(rope, 3);
            tail = store.admit(rope, files);
            if (overview) middle = store.admit(rope, files);
            group = store.admit(rope, files);
        }
        Desktop app(exe, profile, true);
        if (overview) {
            SendMessageW(app.workspace, WM_COMMAND, WPARAM(win::WorkspaceCommand::overview), 0);
            require(until([&] { return app.frame().rows.size() == 2; }), "Missing second overview rope");
        }
        DWORD pid{};
        GetWindowThreadProcessId(app.workspace, &pid);
        for (int answer : {IDCANCEL, IDOK}) {
            select_command(app.workspace, center(app.card(group)), Command::expand, [&](HWND popup) {
                if (review.empty()) return;
                const auto stage = [](win::MenuStage s) { return static_cast<WPARAM>(s); };
                const auto query = [&](WPARAM index) { return SendMessageW(popup, win::menu_timing_query, index, 0); };
                LARGE_INTEGER hz{};
                QueryPerformanceFrequency(&hz);
                const auto composed = query(stage(win::MenuStage::count));
                const auto input = query(stage(win::MenuStage::input));
                require(input > 0 && composed > input, "Missing menu timing receipt");
                const auto view = overview ? "overview" : "single";
                const auto open = answer == IDCANCEL ? "first" : "repeat";
                std::cout << view << ' ' << open << " input_to_composed_ms=" << (composed-input)*1000./hz.QuadPart
                          << " targets=" << query(stage(win::MenuStage::count)+1)
                          << " frames=" << query(stage(win::MenuStage::count)+2) << '\n';
                fs::create_directories(review);
                PaperBackdrop{}.capture(popup, bounds(popup), review / (std::string(view) + "-" + open + ".bmp"));
            });
            HWND confirm{};
            require(until([&] {
                confirm = owned_window(pid, L"#32770");
                return confirm && read(confirm) == L"Expand file group?" && GetDlgItem(confirm, IDOK);
            }), "Tail confirmation missing");
            require(read(GetDlgItem(confirm, 1711)) == (overview ? L"Second" : L"main") &&
                    read(GetDlgItem(confirm, 1712)).find(L"removes 1 card from the end") != std::wstring::npos &&
                    read(GetDlgItem(confirm, IDOK)) == L"Expand group", "Missing affected rope, tail count or action");
            GUITHREADINFO focus{sizeof(focus)};
            require(until([&] { return GetForegroundWindow() == confirm && GetGUIThreadInfo(0, &focus) &&
                                       focus.hwndFocus == GetDlgItem(confirm, IDCANCEL); }), "Cancel did not receive initial focus");
            if (!review.empty() && !overview && answer == IDCANCEL)
                PaperBackdrop{}.capture(confirm, bounds(confirm), review / "expand-confirm.bmp");
            if (overview && answer == IDOK) {
                // Simulate a committed reorder on the disposable profile while the dialog is open.
                const auto path = (profile / "history.db").u8string();
                sqlite3* raw{};
                require(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &raw, SQLITE_OPEN_READWRITE,
                                        nullptr) == SQLITE_OK, "Could not open the concurrent fixture writer");
                std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
                Statement change(raw, "UPDATE entries SET sort_key=(SELECT max(sort_key)+1 FROM entries WHERE rope_id=?) WHERE id=?");
                change.bind(1, rope);
                change.bind(2, middle);
                change.run();
                const auto first_confirm = confirm;
                button(first_confirm, IDOK);
                require(until([&] {
                    confirm = owned_window(pid, L"#32770");
                    return confirm && confirm != first_confirm && IsWindowVisible(confirm) &&
                           GetForegroundWindow() == confirm && GetGUIThreadInfo(0, &focus) &&
                           focus.hwndFocus == GetDlgItem(confirm, IDCANCEL) && GetDlgItem(confirm, IDOK);
                }), "Changed tail did not request fresh consent");
                require(order(profile, rope) == Order{group, tail, middle}, "Stale consent removed a card");
            }
            if (answer == IDCANCEL && !overview) {
                key(VK_RETURN, true);
                key(VK_RETURN, false);
            } else button(confirm, answer);
            require(until([&] { return !IsWindow(confirm); }), "Confirmation stayed open");
            if (answer == IDCANCEL)
                require(order(profile, rope) == (overview ? Order{group, middle, tail} : Order{group, tail}),
                        "Cancel removed existing cards");
        }
        require(until([&] { return app.contains("expand " + group + " committed"); }), "Menu did not expand the group");
        app.stop();
        HistoryStore reopened(profile);
        const auto children = reopened.hanging(rope);
        require(children.size() == (overview ? 4u : 3u) && (!overview || children.back().id == tail) &&
                children[0].id == group && reopened.active_rope() == "main" &&
                reopened.load(children[1].id).snapshot.formats[0].bytes == image_file_sample::drop({first}).bytes &&
                reopened.load(children[2].id).snapshot.formats[0].bytes == image_file_sample::drop({second}).bytes,
                "Expanded children did not reopen on the source rope in file order");
    }

    {
        const auto capacity_profile = root.path / "capacity";
        domain::Id id;
        {
            HistoryStore store(capacity_profile);
            store.set_capacity("main", 2);
            id = store.admit("main", files);
        }
        Desktop app(exe, capacity_profile, true);
        DWORD pid{};
        GetWindowThreadProcessId(app.workspace, &pid);
        for (int close : {IDCANCEL, win::fluent::dialog_close_id, IDOK}) {
            select_command(app.workspace, center(app.card(id)), Command::expand);
            HWND prompt{};
            require(until([&] {
                prompt = owned_window(pid, L"#32770");
                GUITHREADINFO focus{sizeof(focus)};
                return prompt && read(prompt) == L"More rope capacity needed" && IsWindowVisible(prompt) &&
                       GetForegroundWindow() == prompt && GetGUIThreadInfo(0, &focus) &&
                       GetDlgItem(prompt, IDOK) && focus.hwndFocus == GetDlgItem(prompt, IDOK);
            }), "Capacity notice missing");
            require(!GetDlgItem(prompt, IDCANCEL) && read(GetDlgItem(prompt, IDOK)) == L"OK" &&
                    read(GetDlgItem(prompt, 1712)).find(L"at least 3") != std::wstring::npos,
                    "Capacity notice did not explain the minimum or offered expansion");
            if (close == IDCANCEL) {
                if (!review.empty()) PaperBackdrop{}.capture(prompt, bounds(prompt), review / "expand-capacity.bmp");
                key(VK_ESCAPE, true);
                key(VK_ESCAPE, false);
            } else button(prompt, close);
            require(until([&] { return !IsWindow(prompt) && IsWindowEnabled(app.workspace); }) &&
                    order(capacity_profile) == Order{id}, "Capacity notice changed history or left its owner disabled");
        }
    }

    const auto profile = root.path / "floating";
    domain::Id group;
    {
        HistoryStore store(profile);
        group = store.admit("main", files);
        store.detach(group, {"", 20, 20, 240, 160});
    }
    const auto windows = [](DWORD pid) {
        std::vector<RECT> result;
        HWND next{};
        while ((next = FindWindowExW(nullptr, next, L"ClipRope.Floating", nullptr))) {
            DWORD owner{};
            GetWindowThreadProcessId(next, &owner);
            if (pid == owner) result.push_back(bounds(next)); // Includes hidden provisional windows.
        }
        return result;
    };
    std::vector<RECT> before_restart;
    for (const wchar_t* fault : {L"expand-window", L"expand", L""}) {
        Desktop app(exe, profile, false, *fault ? fault : nullptr);
        const HWND source = app.floating_window();
        DWORD pid{};
        GetWindowThreadProcessId(source, &pid);
        const auto initial = bounds(source);
        select_command(source, center(initial), Command::expand);
        if (*fault) {
            require(until([&] { return app.contains("command failed"); }) && windows(pid).size() == 1,
                    "Failed expansion leaked a provisional window");
        } else {
            require(until([&] { return app.contains("expand " + group + " committed"); }), "Floating expansion failed");
            const auto placed = windows(pid);
            before_restart = placed;
            require(placed.size() == 3, "Wrong Floating window count");
            for (std::size_t i = 0; i < placed.size(); ++i)
                for (std::size_t j = 0; j < i; ++j) {
                    RECT overlap{};
                    require(!IntersectRect(&overlap, &placed[i], &placed[j]), "Floating tiles overlap in a free work area");
                }
        }
        auto final_rect = bounds(source);
        require(EqualRect(&initial, &final_rect), "Floating source moved during expansion");
        app.stop();
        HistoryStore store(profile);
        require(store.floating().size() == (*fault ? 1u : 3u) && store.hanging("main").empty(),
                "Wrong durable Floating count after reopen");
        if (!*fault) {
            domain::Layout a, b;
            for (const auto& entry : store.floating()) {
                if (entry.id == group) continue;
                const auto bytes = store.load(entry.id).snapshot.formats[0].bytes;
                if (bytes == image_file_sample::drop({first}).bytes) a = store.layout(entry.id);
                else if (bytes == image_file_sample::drop({second}).bytes) b = store.layout(entry.id);
            }
            require(!a.monitor.empty() && a.monitor == b.monitor && (a.y < b.y || (a.y == b.y && a.x < b.x)),
                    "Floating children were not tiled in file order");
        }
    }
    {
        Desktop app(exe, profile);
        DWORD pid{};
        GetWindowThreadProcessId(app.floating_window(), &pid);
        const auto restored = windows(pid);
        require(restored.size() == before_restart.size(), "Expanded Floating windows did not reopen");
        for (const auto& rect : before_restart)
            require(std::any_of(restored.begin(), restored.end(), [&](const RECT& r) { return EqualRect(&r, &rect); }),
                    "Expanded placement changed on restart");
    }
    const auto replace_profile = root.path / "floating-replace";
    {
        HistoryStore store(replace_profile);
        exec(store.database().handle(), "UPDATE app_settings SET keep_expanded_group=0");
        group = store.admit("main", files);
        store.detach(group, {"", 20, 20, 240, 160});
    }
    for (const auto* fault : {L"expand-window", L"expand", L""}) {
        Desktop app(exe, replace_profile, false, *fault ? fault : nullptr);
        const auto source = app.floating_window();
        DWORD pid{};
        GetWindowThreadProcessId(source, &pid);
        select_command(source, center(bounds(source)), Command::expand);
        if (*fault)
            require(until([&] { return app.contains("command failed") && windows(pid).size() == 1; }) &&
                    IsWindowVisible(source), "Failed replacement dismissed the Floating group");
        else
            require(until([&] { return !IsWindow(source) && windows(pid).size() == 2; }),
                    "Floating replacement did not retire exactly its source");
        app.stop();
        HistoryStore reopened(replace_profile);
        const auto entries = reopened.floating();
        require(entries.size() == (*fault ? 1u : 2u) && (entries.front().id == group) == bool(*fault),
                "Floating replacement or rollback did not survive restart");
    }
    require(sequence == GetClipboardSequenceNumber(), "Expanding through a menu touched the clipboard");
}
