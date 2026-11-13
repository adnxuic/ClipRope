using IdPair = std::pair<domain::Id, domain::Id>;
std::vector<IdPair> main_order(Sql& db) {
    Statement q(db.db, "SELECT id,snapshot_id FROM entries WHERE rope_id='main' ORDER BY sort_key,id");
    std::vector<IdPair> order;
    while (q.step()) order.emplace_back(q.text(0), q.text(1));
    return order;
}
void measure_profiles(const fs::path& exe, const fs::path& output, int trials) {
    struct Scenario { const char* name; unsigned hanging{}, hidden{}, visible{}; bool lock{}; };
    constexpr Scenario scenarios[] = {
        {"empty"}, {"small",4}, {"large",1024}, {"locked_staging",4,0,0,true},
        {"hidden_floating",4,24}, {"mixed_floating",4,20,4}};
    history_fixture::Directory dir;
    fs::create_directories(output);
    std::ofstream csv(output / "arch-profiles.csv");
    csv << "profile,trial,hanging,hidden_floating,shown_floating,host_hwnds,ready_ms,expand_frame_ms,first_commit_ms,"
           "exit_ms,host_private,worker_private,working_set,gdi,user,handles,threads,gpu_dedicated,gpu_shared,"
           "sequence,admitted,snapshot,victim\n" << std::fixed << std::setprecision(4);
    require(bool(csv), "Cannot open restore metrics");
    RawSource source;
    GpuCounters gpu;
    for (const auto& s : scenarios) for (int i = 0; i < trials; ++i) {
        auto profile = dir.path / (std::string(s.name) + "-" + std::to_string(i));
        std::vector<IdPair> seeded;
        if (s.hanging) {
            HistoryStore store(profile);
            seed_order(store, s.hanging);
            for (const auto& e : store.hanging("main")) seeded.emplace_back(e.id, e.snapshot_id);
            for (unsigned n = 0; n < s.visible + s.hidden; ++n) {
                domain::Layout place{"", 350. + n * 4, 280. + n * 3, 240, 160, n < s.visible};
                const auto id = store.create_floating_text(place);
                require(store.layout(id).visible == place.visible, "Floating seed visibility mismatch");
            }
        }
        const auto part = profile / "staging" / (std::string(32, '0') + ".part");
        struct Lock { HANDLE file{INVALID_HANDLE_VALUE}; ~Lock() { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); } } lock;
        if (s.lock) {
            lock.file = CreateFileW(part.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            require(lock.file != INVALID_HANDLE_VALUE, "Cannot hold fixture staging file");
        }
        domain::Id captured, victim;
        {
            Host host(exe, profile);
            Sql sql(profile, host.pid);
            require(sql.active() == "main" && main_order(sql) == seeded && sql.count("PRAGMA user_version") == 11,
                    "Startup changed seeded order, collector or schema");
            unsigned windows{}, visible{}, hidden{};
            HWND hwnd{};
            while ((hwnd = FindWindowExW(nullptr, hwnd, nullptr, nullptr)) != nullptr) {
                DWORD pid{};
                GetWindowThreadProcessId(hwnd, &pid);
                if (pid != host.pid) continue;
                ++windows;
                wchar_t cls[128]{};
                GetClassNameW(hwnd, cls, 128);
                if (std::wstring_view(cls) == L"ClipRope.Floating") {
                    if (GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_VISIBLE) ++visible;
                    else ++hidden;
                }
            }
            require(visible == s.visible && hidden == 0, "Startup materialized the wrong Floating HWNDs");
            require(!s.lock || fs::is_regular_file(part), "Locked disposable file unexpectedly disappeared");
            auto resources = process_tree(host.pid);
            auto counters = gpu.read(resources.ids);
            const auto frames = host.metric(win::PerformanceCounter::painted_frames);
            const auto open = Clock::now();
            host.toggle();
            host.paint();
            require(host.visible() && host.metric(win::PerformanceCounter::painted_frames) > frames,
                    "Restore workload did not present its first workspace frame");
            const auto expanded = ms(open);
            host.toggle();
            const auto elapsed = capture(source, sql, source.text());
            auto actual = main_order(sql);
            require(!actual.empty() && actual.front().first != (seeded.empty() ? "" : seeded.front().first),
                    "First post-ready capture has no new head");
            auto expected = seeded;
            const auto admitted = actual.front();
            expected.insert(expected.begin(), admitted);
            if (expected.size() > static_cast<size_t>(sql.count("SELECT capacity FROM ropes WHERE id='main'"))) {
                victim = expected.back().first;
                expected.pop_back();
            }
            require(actual == expected, "First capture changed survivor identity or discarded the wrong tail");
            captured = admitted.first;
            const auto seq = GetClipboardSequenceNumber();
            const auto close = Clock::now();
            host.exit_cleanly();
            const auto shutdown = ms(close);
            csv << s.name << ',' << i << ',' << seeded.size() << ',' << s.hidden << ',' << visible << ',' << windows << ','
                << host.startup << ',' << expanded << ',' << elapsed << ',' << shutdown << ',' << resources.host_private
                << ',' << resources.worker_private << ',' << resources.working_set << ',' << resources.gdi << ',' << resources.user
                << ',' << resources.handles << ',' << resources.threads << ',' << counters.dedicated << ',' << counters.shared
                << ',' << seq << ',' << captured << ',' << admitted.second << ',' << victim << std::endl;
        }
        if (s.lock) { CloseHandle(lock.file); lock.file = INVALID_HANDLE_VALUE; }
        HistoryStore reopened(profile);
        require(reopened.hanging("main").front().id == captured && reopened.floating().size() == s.visible + s.hidden,
                "Clean exit lost durable population");
        require(!s.lock || !fs::exists(part), "Released staging file was not reclaimed on reopen");
    }
}
