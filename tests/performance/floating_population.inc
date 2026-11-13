void measure_floating_population(const fs::path& exe, const fs::path& output) {
    history_fixture::Directory dir;
    fs::create_directories(output);
    std::ofstream csv(output / "floating-population.csv");
    csv << "population,shown,trial,ready_ms,first_capture_ms,first_reveal_ms,host_hwnds,hidden_hwnds,"
           "user,gdi,host_private,handles\n" << std::fixed << std::setprecision(4);
    MONITORINFOEXA monitor{};
    monitor.cbSize = sizeof(monitor);
    require(GetMonitorInfoA(MonitorFromPoint({0,0}, MONITOR_DEFAULTTOPRIMARY), &monitor), "Monitor unavailable");
    RawSource source;
    struct Seed { domain::Id id, snapshot; domain::Layout layout; };
    for (unsigned size : {24u,64u,256u}) for (unsigned shown : {0u,4u}) for (unsigned trial = 0; trial < 3; ++trial) {
        const auto profile = dir.path / std::to_string(size) / std::to_string(shown) / std::to_string(trial);
        std::vector<Seed> all;
        size_t reveal = shown;
        {
            HistoryStore store(profile);
            for (unsigned i = 0; i < size; ++i) {
                domain::Layout layout{monitor.szDevice,40.+16*(i%8),50.+12*(i%10),240,160,i<shown};
                const auto id = store.create_floating_text(layout);
                all.push_back({id,store.entry(id).snapshot_id,layout});
                if (i >= shown && id < all[reveal].id) reveal = i;
            }
            all[reveal].layout.width = 296;
            store.save_layout(all[reveal].id, all[reveal].layout);
        }
        {
            Host host(exe, profile);
            Sql sql(profile, host.pid);
            const auto capture_ms = capture(source, sql, source.text());
            require(main_order(sql).size() == 1, "First external copy was lost or admitted twice");
            unsigned windows{}, visible{}, hidden{};
            for (HWND hwnd{}; (hwnd = FindWindowExW(nullptr, hwnd, nullptr, nullptr));) {
                DWORD pid{};
                GetWindowThreadProcessId(hwnd,&pid);
                if (pid != host.pid) continue;
                ++windows;
                wchar_t cls[128]{};
                GetClassNameW(hwnd,cls,128);
                if (std::wstring_view(cls) != L"ClipRope.Floating") continue;
                if (GetWindowLongPtrW(hwnd,GWL_STYLE) & WS_VISIBLE) ++visible;
                else ++hidden;
            }
            require(visible == shown && hidden == 0, "Startup did not restore only visible Floating entries");
            const auto resources = process_tree(host.pid);
            const auto menu = hidden_fixture::tray_menu(host.listener, host.pid);
            const auto start = Clock::now();
            hidden_fixture::reveal(menu);
            require(wait([&] {
                for (HWND w{}; (w=FindWindowExW(nullptr,w,L"ClipRope.Floating",nullptr));) {
                    DWORD pid{};
                    RECT rect{};
                    GetWindowThreadProcessId(w,&pid);
                    if (pid==host.pid && (GetWindowLongPtrW(w,GWL_STYLE)&WS_VISIBLE) && GetWindowRect(w,&rect) &&
                        rect.right-rect.left==MulDiv(296,GetDpiForWindow(w),96)) return true;
                }
                return false;
            }), "The tray did not reveal the bound hidden card");
            const auto reveal_ms = ms(start);
            all[reveal].layout.visible = true;
            csv << size << ',' << shown << ',' << trial << ',' << host.startup << ',' << capture_ms << ',' << reveal_ms << ','
                << windows << ',' << hidden << ',' << resources.user << ',' << resources.gdi << ',' << resources.host_private
                << ',' << resources.handles << std::endl;
            host.exit_cleanly();
        }
        HistoryStore reopened(profile);
        require(reopened.floating().size()==size && reopened.hanging("main").size()==1, "Restart lost membership");
        for (const auto& item : all) {
            const auto entry = reopened.entry(item.id);
            const auto layout = reopened.layout(item.id);
            require(!entry.rope_id && entry.snapshot_id==item.snapshot && layout.monitor==item.layout.monitor &&
                    layout.visible==item.layout.visible && std::abs(layout.x-item.layout.x)<=1 &&
                    std::abs(layout.y-item.layout.y)<=1 && std::abs(layout.width-item.layout.width)<=1 &&
                    std::abs(layout.height-item.layout.height)<=1, "Restart changed hidden card identity or placement");
        }
    }
    require(bool(csv), "Could not finish Floating CSV");
}
