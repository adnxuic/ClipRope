// Separate controls for a real movement that changes only the first two ranks.
// Use the same full-rope population and shared sealed snapshot as --arch-order.
void measure_adjacent_order(const fs::path& output, int trials) {
    history_fixture::Directory dir;
    fs::create_directories(output);
    std::ofstream csv(output / "history-adjacent.csv");
    require(bool(csv), "Cannot open adjacent control CSV");
    csv << "population,action,trial,ms,changed_rows,traced_statements,target,head,second,tail,version\n"
        << std::fixed << std::setprecision(4);
    for (unsigned population : {48u, 256u, 1024u, 4096u}) {
        const auto profile = dir.path / std::to_string(population);
        std::vector<domain::Id> expected;
        domain::Id sealed;
        domain::Entry final_head, protected_entry;
        {
            HistoryStore store(profile);
            sealed = seed_order(store, population).snapshot_id;
            const auto neighbor = store.create_rope("Neighbor", 1);
            protected_entry = store.entry(store.admit(neighbor, snapshot(sample::formats(true))));
            for (const auto& entry : store.hanging("main")) expected.push_back(entry.id);
            require(expected.size() == population, "Wrong adjacent control population");
            unsigned follows{};
            store.on_head_change = [&](const auto&) { ++follows; };
            auto* db = store.database().handle();
            const auto version = [&] {
                Statement row(db, "SELECT version FROM rope_view_state WHERE rope_id='main'");
                require(row.step(), "Missing adjacent control viewport");
                return row.integer(0);
            };
            for (int trial = 0; trial <= trials; ++trial) {
                for (bool promote : {true, false}) {
                    const auto target = store.entry(expected[promote ? 1 : 0]);
                    const auto prior_version = version();
                    const auto prior_changes = sqlite3_total_changes64(db);
                    const auto prior_follows = follows;
                    int traced{};
                    if (trial == trials)
                        sqlite3_trace_v2(db, SQLITE_TRACE_STMT, [](unsigned, void* count, void*, void*) {
                            ++*static_cast<int*>(count);
                            return 0;
                        }, &traced);
                    const auto start = Clock::now();
                    if (promote) store.promote(target.id, &target);
                    else store.reorder(target.id, 1, &target);
                    const auto elapsed = ms(start);
                    if (trial == trials) sqlite3_trace_v2(db, 0, nullptr, nullptr);
                    const auto changes = sqlite3_total_changes64(db) - prior_changes;
                    const auto after_version = version();
                    require(after_version == prior_version + 1 && follows == prior_follows + !promote,
                            "Adjacent control changed viewport or publication semantics");
                    std::swap(expected[0], expected[1]);
                    const auto ordered = store.hanging("main");
                    require(ordered.size() == expected.size(), "Adjacent control changed membership");
                    for (size_t n = 0; n < ordered.size(); ++n)
                        require(ordered[n].id == expected[n] && ordered[n].snapshot_id == sealed &&
                                ordered[n].sort_key == static_cast<std::int64_t>(n), "Adjacent control changed identity or raw order");
                    csv << population << ',' << (promote ? "promote_second" : "reorder_second") << ',' << trial
                        << ',' << elapsed << ',' << changes << ',' << (trial == trials ? traced : -1) << ','
                        << target.id << ',' << expected[0] << ',' << expected[1] << ',' << expected.back() << ',' << after_version << '\n';
                }
            }
            const auto victim = expected.back();
            expected.pop_back();
            final_head = store.entry(store.admit("main", snapshot(sample::formats(true))));
            expected.insert(expected.begin(), final_head.id);
            history_fixture::rejects([&] { store.entry(victim); }, "Wrong adjacent control victim");
        }
        HistoryStore reopened(profile);
        verify_order_reopen(reopened, expected, sealed, final_head, protected_entry);
    }
    require(bool(csv), "Could not finish adjacent control CSV");
}
