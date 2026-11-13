#include "history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "platform_win/search_text.hpp"
#include <chrono>
#include <iostream>

using namespace cliprope;
using history_fixture::require;
int main() {
    try {
        history_fixture::Directory dir;
        domain::Snapshot snapshot;
        for (const auto& [name,text] : {std::pair{"Rich Text Format", "{\\rtf1\\ansi recovered text}"},
                                        std::pair{"HTML Format", "<html><!--StartFragment-->body<!--EndFragment--></html>"}}) {
            domain::Format f;
            f.name = name;
            f.bytes.assign(text,text+std::char_traits<char>::length(text));
            snapshot.formats.push_back(std::move(f));
        }
        snapshot.source_hint = "synthetic source";
        std::string blocked;
        const auto expected = win::extract_search_fields(snapshot);
        require(!expected.empty(), "Synthetic rich fields empty");
        domain::Id id, sibling;
        {
            sqlite3* observer{};
            using Clock = std::chrono::steady_clock;
            Clock::time_point started{};
            double commit_ms{};
            unsigned conversions{};
            persistence::HistoryStore store(dir.path,[&](std::string_view step) {
                if (step == "admission-write-start") started=Clock::now();
                if (step == "committed" && started != Clock::time_point{}) {
                    commit_ms=std::chrono::duration<double,std::milli>(Clock::now()-started).count();
                    started={};
                }
                if (step != "search-rich-edit-create" && step!="search-html-convert") return;
                ++conversions;
                require(observer != nullptr && sqlite3_exec(observer,"BEGIN IMMEDIATE; ROLLBACK",nullptr,nullptr,nullptr)==SQLITE_OK,
                        "Rich conversion held the SQLite write lock");
                if (step == blocked) throw std::runtime_error("Injected rich conversion failure");
            });
            auto path=(dir.path/"history.db").u8string();
            require(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()),&observer,SQLITE_OPEN_READWRITE,nullptr)==SQLITE_OK,
                    "Cannot observe the write lock");
            const std::unique_ptr<sqlite3,decltype(&sqlite3_close)> connection(observer,sqlite3_close);
            for (auto stage : {"search-rich-edit-create","search-html-convert"}) {
                blocked=stage;
                history_fixture::rejects([&] { store.admit_capture("main",snapshot); },"Faulted extraction committed");
                require(store.hanging("main").empty(),"Faulted search preparation created an entry");
            }
            blocked.clear();
            id=store.admit_capture("main",snapshot);
            sibling=store.copy(id,"main");
            require(conversions==5 && commit_ms>0, "Rich adapter or transaction observation unavailable");
            std::cout << "conversion_points=" << conversions << " admission_writer_ms=" << commit_ms << '\n';
        }
        persistence::HistoryStore reopened(dir.path);
        const auto saved=reopened.entry(id).snapshot_id;
        require(reopened.entry(sibling).snapshot_id==saved,"Shared snapshot changed at reopen");
        application::SearchFields actual;
        persistence::Statement rows(reopened.database().handle(),"SELECT kind,search_text FROM search_fields WHERE snapshot_id=? ORDER BY ordinal");
        rows.bind(1,saved);
        while (rows.step()) actual.emplace_back(static_cast<application::SearchField>(rows.integer(0)),rows.wide(1));
        require(actual==expected,"Prepared fields differ after commit and reopen");
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
