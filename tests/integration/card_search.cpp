#include "persistence/card_search.hpp"
#include "persistence/history_store.hpp"
#include "history_fixture.hpp"
#include <cstring>
#include <iostream>
using namespace cliprope;
using history_fixture::require;
namespace {
domain::Snapshot text(std::wstring value) {
    domain::Format f;
    f.standard_id = 13;
    f.bytes.resize((value.size() + 1) * 2);
    std::memcpy(f.bytes.data(), value.data(), value.size() * 2);
    return {{std::move(f)}};
}
void run() {
    history_fixture::Directory dir;
    persistence::HistoryStore store(dir.path);
    const auto last = store.admit("main", text(std::wstring(3799, L'x') + L"😀磁性 %_ 磁性 TeSt 磁性"));
    for (int i = 1; i < 100; ++i)
        store.admit("main", text(L"other"));
    persistence::CardSearch search(dir.path);
    application::FindScope scope{"main"};
    auto hits = search.query(scope, L"磁性");
    require(hits.total == 3 && store.hanging("main").back().id == last, "Deep matches or numbering lost");
    auto a = search.locate(hits, 0), b = search.locate(hits, 2);
    require(a && b && a->entry == last && a->range.start == 3801 && a->range.length == 2 &&
                b->range.start > a->range.start && a->context.substr(a->range.start - a->context_start, 2) == L"磁性",
            "Wrong UTF-16 match range");
    require(search.query(scope, L"%_").total == 1 && search.query(scope, L"TEST").total == 1,
            "Literal or ASCII search failed");
    auto work = store.create_rope("Work", 10), notes = store.create_rope("Notes", 10);
    auto work_id = store.admit(work, text(L"磁性磁性磁性磁性磁性"));
    store.admit(notes, text(L"磁性磁性"));
    store.set_capacity("main", 101);
    store.admit("main", text(L"磁性"));
    store.admit(domain::rebin_id, text(L"磁性"));
    auto visible = search.query({work, true, false}, L"磁性");
    require(visible.total == 11 && search.locate(visible, 0)->entry == work_id &&
                search.locate(visible, 5)->rope == notes && search.locate(visible, 7)->rope == "main",
            "Wrong overview circle");
    require(search.query({work, true, true}, L"磁性").total == 12, "Visible ReBin missing");
    store.detach(work_id, {});
    require(search.query({work, true, false}, L"磁性").total == 6 && !search.locate(visible, 0), "Stale Floating hit");
    require(search.query(scope, L"磁性").total == 4, "Lost matches in external text");
    domain::Format rtf;
    rtf.name = "Rich Text Format";
    std::string source = R"({\rtf1\ansi hello \u30913? mixed})";
    rtf.bytes.assign(source.begin(), source.end());
    store.admit("main", {{rtf}});
    require(search.query(scope, L"hello").total == 1, "RTF-only body unavailable");
    auto rich = text(L"Saved visible label");
    domain::Format html;
    html.name = "HTML Format";
    std::string fragment = "<a href='https://search.example/atlas?a=1&amp;b=2'>Saved visible label</a>";
    std::string header = "StartFragment:0000000000\r\nEndFragment:0000000000\r\n";
    char offsets[128]{};
    std::snprintf(offsets, sizeof(offsets), "StartFragment:%010u\r\nEndFragment:%010u\r\n", unsigned(header.size()),
                  unsigned(header.size() + fragment.size()));
    auto markup = std::string(offsets) + fragment;
    html.bytes.assign(markup.begin(), markup.end());
    rich.formats.push_back(html);
    rich.source_hint = "EXCEL.EXE";
    auto r = store.admit(work, rich);
    auto copy = store.copy(r, notes);
    const application::FindScope all{work, true, false};
    require(search.query(all, L"visible label").total == 2 && search.query(all, L"EXCEL.EXE").total == 2 &&
                search.query(all, L"atlas?a=1&b=2").total == 2,
            "Shared Office text, label or saved link is missing or doubled");
    auto old = search.query(all, L"visible label");
    auto expected = store.entry(r);
    store.save_text(r, expected.revision, L"Changed title");
    require(!search.locate(old, 0) && search.locate(old, 1)->entry == copy &&
                search.query(all, L"changed title").total == 1,
            "Edit altered a shared snapshot or exposed a stale hit");
    const auto fresh = store.entry(r);
    history_fixture::rejects([&] { store.save_text(r, expected.revision, L"Never committed"); }, "Stale edit accepted");
    require(!search.query(all, L"Never committed").total && store.entry(r).revision == fresh.revision,
            "Rejected edit changed the search index");
    store.erase(copy);
    require(!search.locate(old, 1) && !search.query(all, L"visible label").total, "Deleted result survived");
    domain::Format files;
    files.standard_id = 15;
    files.bytes.resize(20);
    files.bytes[0] = 20;
    files.bytes[16] = 1;
    for (std::wstring path : {L"Z:\\absent\\First.png", L"Z:\\absent\\Second.png"}) {
        auto raw = reinterpret_cast<const unsigned char*>(path.c_str());
        files.bytes.insert(files.bytes.end(), raw, raw + (path.size() + 1) * 2);
    }
    files.bytes.resize(files.bytes.size() + 2);
    store.admit(work, {{files}});
    require(search.query(all, L"absent").total == 2 && search.query(all, L"second.png").total == 1,
            "File path fields skipped or duplicated a leaf");
    domain::Format descriptor;
    descriptor.name = "FileGroupDescriptorW";
    descriptor.bytes.resize(4 + 592 * 2);
    descriptor.bytes[0] = 2;
    std::memcpy(descriptor.bytes.data() + 4 + 72, L"First.csv", 18);
    std::memcpy(descriptor.bytes.data() + 4 + 592 + 72, L"Later.xlsx", 20);
    store.admit(work, {{descriptor}});
    require(search.query(all, L"Later.xlsx").total == 1, "Virtual file beyond first descriptor was skipped");
    auto heavy = store.admit(domain::rebin_id, text(std::wstring(1000000, L'a')));
    auto frequent = search.query({domain::rebin_id, false, true}, L"aa");
    require(frequent.total == 500000, "Frequent nonoverlapping count was truncated");
    for (size_t occurrence : {0, 63, 64, 135, 499999, 32}) {
        auto found = search.locate(frequent, occurrence);
        require(found && found->entry == heavy && found->range.start == occurrence * 2, "Position page seek failed");
    }
}
} // namespace
int main() {
    try {
        run();
        std::cout << "Deep history, UTF-16, literals, RTF and indexed content passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
