// The desktop receives its normal WM_CONTEXTMENU at its own reported hit point.
// The QPC interval ends at its synchronous painted mark, before any driver query.
void private_menu_benchmark(const fs::path& exe, const fs::path& folder, bool quick,
                            localization::Language language) {
    require(probe::private_station(), "Private menu benchmark requires the guarded window station");
    fs::create_directories(folder);
    std::ofstream csv(folder / "private-menus.csv");
    require(bool(csv), "Cannot write private menu measurements");
    csv << "language,entry,process_trial,opening,repeat,host_pid,ready_ms,qpc_hz,posted_qpc,input_qpc,focus_qpc,"
           "model_qpc,controls_qpc,frame_qpc,painted_qpc,posted_to_paint_ms,input_to_paint_ms,"
           "tree_kernel_before,tree_kernel_open,tree_kernel_closed,tree_gdi_before,tree_gdi_open,tree_gdi_closed,"
           "tree_user_before,tree_user_open,tree_user_closed,host_private_before,host_private_open,host_private_closed,"
           "kernel_open_delta,kernel_closed_delta,gdi_open_delta,gdi_closed_delta,user_open_delta,user_closed_delta,"
           "target_generations,frame_generations\n";
    LARGE_INTEGER frequency{};
    require(QueryPerformanceFrequency(&frequency) != FALSE, "QPC frequency unavailable");
    history_fixture::Directory directory;
    const auto profile = directory.path / "menu-profile";
    domain::Id rope, entry;
    {
        HistoryStore store(profile);
        save_language(store.database(), language);
        rope = store.create_rope("Menu fixture", 48);
        store.set_active_rope(rope);
        entry = store.admit(rope, snapshot(sample::formats(false)));
    }
    for (const bool rope_menu : {false, true}) {
        const auto target = rope_menu ? win::PerformanceCounter::first_rope_point : win::PerformanceCounter::first_card_point;
        const auto cls = rope_menu ? L"ClipRope.RopeMenu" : L"ClipRope.CardMenu";
        for (int trial = 1; trial <= (quick ? 1 : 5); ++trial) {
            Host host(exe, profile);
            Sql observed(profile, host.pid);
            host.toggle();
            require(wait([&] { return host.visible(); }), "Private menu workspace did not expand");
            LRESULT hit{};
            require(wait([&] { return (hit = host.metric(target)) != 0; }), "Private menu hit point unavailable");
            const DWORD sequence = GetClipboardSequenceNumber();
            const HWND clipboard_owner = GetClipboardOwner();
            for (int repeat = 0; repeat <= (quick ? 2 : 10); ++repeat) {
                const auto before = process_tree(host.pid);
                LARGE_INTEGER posted{};
                QueryPerformanceCounter(&posted);
                require(PostMessageW(host.workspace, WM_CONTEXTMENU, WPARAM(host.workspace), hit) != FALSE,
                        "Could not post the real menu request");
                HWND popup{};
                LRESULT painted{};
                require(wait([&] {
                    popup = owned(host.pid, cls);
                    return popup && (GetWindowLongPtrW(popup, GWL_STYLE) & WS_VISIBLE) &&
                        (painted = send(popup, win::menu_timing_query, WPARAM(win::MenuStage::painted))) >= posted.QuadPart;
                }), "Missing menu painted receipt");
                HWND control = GetDlgItem(popup, rope_menu ? 11901 : 1802); // Rope name and Card Bold.
                require(control != nullptr, "Incorrect native property menu");
                if (rope_menu) {
                    wchar_t name[40]{};
                    SendMessageW(control, WM_GETTEXT, std::size(name), LPARAM(name));
                    require(std::wstring_view(name) == L"Menu fixture", "Unexpected rope opened by hit point");
                }
                const auto open = process_tree(host.pid);
                std::array<LRESULT, size_t(win::MenuStage::count)> marks{};
                auto previous = posted.QuadPart;
                for (size_t i = 0; i < marks.size(); ++i) {
                    marks[i] = send(popup, win::menu_timing_query, i);
                    require(marks[i] >= previous, "Private menu reported out-of-order QPC stages");
                    previous = marks[i];
                }
                require(marks.back() == painted, "Menu painted receipt changed during measurement");
                // Target and frame generation queries are implemented by qualification builds.
                // Leave zero/unsupported counters empty on Production.
                const auto targets = send(popup, win::menu_timing_query, WPARAM(win::MenuStage::count) + 1);
                const auto frames = send(popup, win::menu_timing_query, WPARAM(win::MenuStage::count) + 2);
                require(PostMessageW(popup, WM_CLOSE, 0, 0) != FALSE, "Could not close measured menu");
                require(wait([&] { return !IsWindow(popup) && !owned(host.pid, L"ClipRope.CardMenuFrame"); }),
                        "Private menu did not release its frame");
                const auto closed = process_tree(host.pid);
                require(observed.active() == rope && observed.head(rope) == entry &&
                            sequence == GetClipboardSequenceNumber() && clipboard_owner == GetClipboardOwner(),
                        "Menu measurement changed the bound history or clipboard");
                const auto delta = [](auto after, auto start) { return int64_t(after) - int64_t(start); };
                csv << localization::language_code(language) << ',' << (rope_menu ? "rope" : "card") << ',' << trial
                    << ',' << (repeat ? "repeat" : "first") << ',' << repeat << ',' << host.pid << ',' << host.startup
                    << ',' << frequency.QuadPart << ',' << posted.QuadPart;
                for (auto mark : marks) csv << ',' << mark;
                csv << ',' << (painted - posted.QuadPart) * 1000. / frequency.QuadPart
                    << ',' << (painted - marks.front()) * 1000. / frequency.QuadPart
                    << ',' << before.handles << ',' << open.handles << ',' << closed.handles
                    << ',' << before.gdi << ',' << open.gdi << ',' << closed.gdi
                    << ',' << before.user << ',' << open.user << ',' << closed.user
                    << ',' << before.host_private << ',' << open.host_private << ',' << closed.host_private
                    << ',' << delta(open.handles, before.handles) << ',' << delta(closed.handles, before.handles)
                    << ',' << delta(open.gdi, before.gdi) << ',' << delta(closed.gdi, before.gdi)
                    << ',' << delta(open.user, before.user) << ',' << delta(closed.user, before.user)
                    << ',' << (targets ? std::to_string(targets) : "") << ',' << (frames ? std::to_string(frames) : "")
                    << std::endl;
            }
            host.exit_cleanly();
        }
    }
}
