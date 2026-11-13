// The driver owns the synthetic clipboard and the read-only observer. Only the
// desktop and its worker descendants contribute to the resource samples.
void footprint_images(const fs::path& exe, const fs::path& output) {
    history_fixture::Directory dir;
    fs::create_directories(output);
    std::ofstream csv(output / "images.csv");
    require(bool(csv), "Cannot create image footprint log");
    csv << "series,trial,ms,host_private,worker_private,group_private,host_ws,worker_ws,worker_peak_commit\n";
    const auto profile = dir.path / "images";
    const auto accepted = dib(2048, 42);
    const auto rejected = dib(3072, 43);
    std::string last;
    {
        Host h(exe, profile);
        Sql sql(profile, h.pid);
        RawSource source;
        capture(source, sql, source.text());
        for (unsigned trial = 0; trial < 20; ++trial) {
            Peak peak(h.pid);
            const auto elapsed = capture(source, sql, accepted);
            peak.finish();
            csv << (trial ? "repeat" : "first") << ',' << trial << ',' << elapsed << ',' << peak.host
                << ',' << peak.worker << ',' << peak.total << ',' << peak.host_ws << ',' << peak.worker_ws
                << ',' << peak.worker_commit << std::endl;
        }
        last = sql.head();
        for (unsigned trial = 0; trial < 5; ++trial) {
            Peak peak(h.pid);
            auto start = Clock::now();
            source.put(rejected);
            const auto sequence = GetClipboardSequenceNumber();
            require(wait([&] { return health(h.listener, win::PerformanceCounter::capture_completed_sequence) == sequence; }),
                    "Rejected image did not produce a receipt");
            const auto elapsed = ms(start);
            peak.finish();
            require(sql.head() == last && !health(h.listener, win::PerformanceCounter::capture_saved_count) &&
                    !health(h.listener, win::PerformanceCounter::capture_worker_exit) && peak.worker_commit,
                    "Rejected image changed history or worker failed");
            csv << "reject," << trial << ',' << elapsed << ',' << peak.host << ',' << peak.worker << ',' << peak.total
                << ',' << peak.host_ws << ',' << peak.worker_ws << ',' << peak.worker_commit << std::endl;
        }
        h.exit_cleanly();
    }
    const auto cli = exe.parent_path() / "cliprope_headless.exe";
    HANDLE readback = probe::launch({cli.wstring(), profile.wstring(), L"head"}, probe::desktop_path());
    require(readback, "Cannot reopen and publish the saved image");
    DWORD code = 1;
    const bool finished = wait([&] { return WaitForSingleObject(readback, 0) == WAIT_OBJECT_0; }, 10000);
    GetExitCodeProcess(readback, &code);
    CloseHandle(readback);
    require(finished && !code && OpenClipboard(nullptr), "Image publication after restart failed");
    {
        struct Close { ~Close() { CloseClipboard(); } } close;
        const auto data = GetClipboardData(CF_DIB);
        const void* bytes = data ? GlobalLock(data) : nullptr;
        const bool same = bytes && GlobalSize(data) >= accepted.bytes.size() &&
                          !std::memcmp(bytes, accepted.bytes.data(), accepted.bytes.size());
        if (bytes) GlobalUnlock(data);
        require(same, "Republished DIB bytes changed after restart");
        require(probe::private_station() && EmptyClipboard(), "Cannot retire the verified synthetic DIB");
    }
    // Each process starts without resident thumbnails. Reopening the same
    // directory also covers the on-disk derivative hit.
    for (unsigned trial = 0; trial < 5; ++trial) {
        auto pictures = dir.path / ("files-" + std::to_string(trial));
        // seed_pictures also creates its photos folder under the profile parent.
        fs::create_directories(pictures);
        const auto store = pictures / "history";
        seed_pictures(store);
        for (bool reopened : {false, true}) {
            Host h(exe, store);
            auto start = Clock::now();
            h.toggle();
            h.paint();
            require(wait([&] { return h.metric(win::PerformanceCounter::thumbnail_bytes) > 0; }),
                    "First file thumbnail did not become resident");
            csv << (reopened ? "thumbnail_reopen" : "thumbnail_first") << ',' << trial << ',' << ms(start)
                << ",0,0,0,0,0,0" << std::endl;
            h.exit_cleanly();
        }
    }
}
