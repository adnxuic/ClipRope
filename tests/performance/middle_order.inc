void measure_middle_order(const fs::path& output) {
    history_fixture::Directory dir;
    fs::create_directories(output);
    std::ofstream csv(output / "middle-order.csv");
    require(bool(csv), "Cannot create middle order CSV");
    csv << "rope,population,action,trial,call_ms,changed_rows,statements,rank_updates,trigger_events\n" << std::fixed << std::setprecision(4);
    for (unsigned size : {100u, 1024u, 4096u}) for (const std::string rope : {std::string("main"),std::string(domain::rebin_id)}) {
        const auto profile = dir.path / (rope == "main" ? "ordinary" : "rebin") / std::to_string(size);
        std::vector<domain::Id> ids;
        auto verify = [&](HistoryStore& store) {
            const auto entries = store.hanging(rope);
            require(entries.size() == ids.size(), "Measured rope changed population");
            for (size_t i = 0; i < ids.size(); ++i)
                require(entries[i].id == ids[i] && entries[i].rope_id == rope, "Measured rope changed order or ownership");
        };
        bool reject{};
        {
            HistoryStore store(profile, [&](auto stage) {
                if (reject && stage == "transaction") throw std::runtime_error("Injected middle-order rollback");
            });
            seed_order(store, size, rope);
            for (const auto& e : store.hanging(rope)) ids.push_back(e.id);
            auto* db = store.database().handle();
            auto measure = [&](const char* action, int trial, auto operation) {
                struct Counts { int statements{}, ranks{}, triggers{}; } counts;
                const bool trace = trial == 15;
                if (trace) sqlite3_trace_v2(db, SQLITE_TRACE_STMT, [](unsigned, void* state, void* stmt, void* detail) {
                    auto& counts = *static_cast<Counts*>(state);
                    if (detail && std::string_view(static_cast<const char*>(detail)).starts_with("--")) {
                        ++counts.triggers;
                        return 0;
                    }
                    ++counts.statements;
                    std::string_view sql(sqlite3_sql(static_cast<sqlite3_stmt*>(stmt)));
                    if (sql.find("UPDATE entries SET sort_key") != sql.npos) ++counts.ranks;
                    return 0;
                }, &counts);
                const auto before = sqlite3_total_changes64(db);
                const auto start = Clock::now();
                operation();
                const auto elapsed = ms(start);
                if (trace) sqlite3_trace_v2(db, 0, nullptr, nullptr);
                csv << rope << ',' << size << ',' << action << ',' << trial << ',' << elapsed << ','
                    << sqlite3_total_changes64(db)-before << ',' << (trace ? counts.statements : -1) << ','
                    << (trace ? counts.ranks : -1) << ',' << (trace ? counts.triggers : -1) << '\n';
            };
            const auto mid = size / 2;
            for (int trial = 0; trial <= 15; ++trial) {
                auto moving = store.entry(ids[mid]);
                measure("middle_move", trial, [&] { store.reorder(moving.id, mid + 1, &moving); });
                std::swap(ids[mid],ids[mid+1]);
                verify(store);
                const auto inserted = ids.back();
                store.detach(inserted);
                ids.pop_back();
                const auto bound = store.entry(inserted);
                measure("middle_insert", trial, [&] { store.rehang_at(inserted, rope, mid, &bound); });
                ids.insert(ids.begin()+mid, inserted);
                verify(store);
                store.promote(ids.front());
                measure("repeat_head", trial, [&] { store.promote(ids.front()); });
                verify(store);
            }
            const auto extra = store.create_floating_text({"",40,50,240,160});
            const auto original = store.entry(extra);
            const auto layout = store.layout(extra);
            reject = true;
            history_fixture::rejects([&] { store.rehang_at(extra, rope, mid, &original); }, "Middle insertion ignored rollback");
            reject = false;
            verify(store);
            require(!store.entry(extra).rope_id && store.layout(extra).width == layout.width, "Rollback lost Floating state");
            store.rehang_at(extra, rope, mid, &original);
            ids.insert(ids.begin()+mid,extra);
            auto trim = [&] {
                if (rope != "main") return;
                const auto tail = ids.back();
                ids.pop_back();
                history_fixture::rejects([&] { store.entry(tail); }, "Full return retained the prior tail");
            };
            trim();
            verify(store);
            const auto neighbor = store.create_rope("Neighbor", 2);
            const auto other = store.entry(store.admit(neighbor, snapshot(sample::formats(true))));
            store.move(other.id, rope, &other, mid);
            ids.insert(ids.begin()+mid,other.id);
            trim();
            require(store.hanging(neighbor).empty(), "Transfer left its source entry behind");
            verify(store);
        }
        HistoryStore reopened(profile);
        verify(reopened);
    }
    require(bool(csv), "Could not finish middle order CSV");
}
