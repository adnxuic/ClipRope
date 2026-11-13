// Fixture construction and order checks are outside each timed transaction.
domain::Entry seed_order(HistoryStore& store, unsigned population, std::string_view rope = "main") {
    return order_fixture::seed(store, population, rope, snapshot(sample::formats(false)));
}

void verify_order_reopen(HistoryStore& store, const std::vector<domain::Id>& expected, const domain::Id& shared,
                         const domain::Entry& new_head, const domain::Entry& protected_entry) {
    const auto main = store.hanging("main");
    require(store.active_rope() == "main" && !main.empty() && main.size() == expected.size() &&
            main.front().id == new_head.id && protected_entry.rope_id, "Restart changed collector, population or head");
    for (size_t i = 0; i < expected.size(); ++i)
        require(main[i].id == expected[i] && main[i].snapshot_id == (i ? shared : new_head.snapshot_id),
                "Restart changed measured order or snapshot identities");
    const auto neighbor = store.hanging(*protected_entry.rope_id);
    require(neighbor.size() == 1 && neighbor.front().id == protected_entry.id &&
            neighbor.front().snapshot_id == protected_entry.snapshot_id, "Restart changed the protected neighbor");
    const auto reference = snapshot(sample::formats(true));
    for (const auto& id : {new_head.id, protected_entry.id}) {
        const auto saved = store.load(id);
        require(saved.snapshot.formats.size() == reference.formats.size(), "Restart lost a measured representation");
        for (size_t i = 0; i < reference.formats.size(); ++i) {
            const auto& actual = saved.snapshot.formats[i];
            const auto& wanted = reference.formats[i];
            require(actual.outcome == "saved" && actual.standard_id == wanted.standard_id &&
                    actual.name == wanted.name && actual.bytes == wanted.bytes, "Restart changed measured format bytes");
        }
    }
}

void measure_order(const fs::path& output, int trials) {
    history_fixture::Directory dir;
    fs::create_directories(output);
    std::ofstream csv(output / "history-order.csv");
    require(bool(csv), "Cannot open history-order CSV");
    csv << "population,action,trial,ms,changed_rows,traced_statements,target,head,tail,version\n"
        << std::fixed << std::setprecision(4);
    for (unsigned population : {48u, 256u, 1024u, 4096u}) {
        const auto profile = dir.path / std::to_string(population);
        domain::Entry final_head, protected_entry;
        domain::Id first_snapshot;
        std::vector<domain::Id> expected;
        {
            HistoryStore store(profile);
            auto db = store.database().handle();
            first_snapshot = seed_order(store, population).snapshot_id;
            auto neighbor = store.create_rope("Neighbor", 1);
            protected_entry = store.entry(store.admit(neighbor, snapshot(sample::formats(true))));
            for (const auto& e : store.hanging("main")) expected.push_back(e.id);
            unsigned follows{};
            store.on_head_change = [&](const auto&) { ++follows; };
            auto verify = [&] {
                const auto entries = store.hanging("main");
                require(entries.size() == expected.size(), "Order benchmark changed membership");
                for (size_t i = 0; i < expected.size(); ++i)
                    require(entries[i].id == expected[i] && entries[i].snapshot_id == first_snapshot,
                            "Order benchmark changed an ID, position or shared snapshot");
            };
            auto version = [&] {
                Statement row(db, "SELECT version FROM rope_view_state WHERE rope_id='main'");
                require(row.step(), "Missing rope version");
                return row.integer(0);
            };
            for (int trial = 0; trial <= trials; ++trial) {
                for (const std::string_view action : {"already_head", "promote_tail", "reorder_tail"}) {
                    const auto target = store.entry(action == "promote_tail" ? expected.back() : expected.front());
                    const auto before = sqlite3_total_changes64(db);
                    const auto prior_version = version();
                    const auto prior_follows = follows;
                    int traced{};
                    if (trial == trials)
                        sqlite3_trace_v2(db, SQLITE_TRACE_STMT, [](unsigned, void* counter, void*, void*) {
                            ++*static_cast<int*>(counter);
                            return 0;
                        }, &traced);
                    const auto start = Clock::now();
                    if (action == "reorder_tail") store.reorder(target.id, expected.size() - 1, &target);
                    else store.promote(target.id, &target);
                    const auto elapsed = ms(start);
                    if (trial == trials) sqlite3_trace_v2(db, 0, nullptr, nullptr);
                    const auto changes = sqlite3_total_changes64(db) - before;
                    if (action == "promote_tail") std::rotate(expected.begin(), expected.end() - 1, expected.end());
                    if (action == "reorder_tail") std::rotate(expected.begin(), expected.begin() + 1, expected.end());
                    verify();
                    const auto after_version = version();
                    require(after_version == prior_version + 1 && follows == prior_follows + (action == "reorder_tail"),
                            "Command version or head-change callback count changed");
                    csv << population << ',' << action << ',' << trial << ',' << elapsed << ',' << changes << ','
                        << (trial == trials ? traced : -1) << ',' << target.id << ',' << expected.front() << ','
                        << expected.back() << ',' << after_version << '\n';
                }
            }
            auto old_tail = expected.back();
            final_head = store.entry(store.admit("main", snapshot(sample::formats(true))));
            expected.pop_back();
            expected.insert(expected.begin(), final_head.id);
            history_fixture::rejects([&] { store.entry(old_tail); }, "Next admission did not evict the measured tail");
            const auto others = store.hanging(neighbor);
            require(store.active_rope() == "main" && others.size() == 1 && others.front().id == protected_entry.id,
                    "Order workload changed the other rope");
        }
        HistoryStore reopened(profile);
        verify_order_reopen(reopened, expected, first_snapshot, final_head, protected_entry);
    }
    require(bool(csv), "Could not finish history-order CSV");
}
