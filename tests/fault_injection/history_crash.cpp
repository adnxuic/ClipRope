#include "../integration/history_fixture.hpp"
#include "persistence/history_store.hpp"
#include <algorithm>
#include <iostream>

using namespace cliprope;
using history_fixture::require;
namespace fs = std::filesystem;
namespace {
constexpr DWORD interrupted = 73;
domain::Snapshot content(unsigned char marker, size_t size = 16000) {
    domain::Format format;
    format.name = "Synthetic Binary";
    format.bytes.assign(size,marker);
    domain::Snapshot snapshot;
    snapshot.formats.push_back(std::move(format));
    return snapshot;
}
void child_crash(const fs::path& root, std::wstring_view boundary) {
    persistence::HistoryStore store(root,[&](std::string_view point){
        if (std::equal(point.begin(), point.end(), boundary.begin(), boundary.end())) {
            // Terminate the writer without C++ unwinding, sqlite3_close or file cleanup.
            TerminateProcess(GetCurrentProcess(),interrupted);
            std::abort();
        }
    });
    store.admit("main",content('N'));
    store.gc();
    throw std::logic_error("Requested interruption boundary was not reached");
}
void launch(const fs::path& root,const std::string& boundary) {
    wchar_t module[32768]{};
    const auto n=GetModuleFileNameW(nullptr,module,32768);
    require(n && n<32768,"Test module path unavailable");
    const std::wstring point(boundary.begin(),boundary.end());
    std::wstring command=L"\"" + std::wstring(module) + L"\" --interrupt \"" + root.wstring() + L"\" " + point;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION proc{};
    require(CreateProcessW(module,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,
                           &startup,&proc)!=0,"Could not launch interruption worker");
    CloseHandle(proc.hThread);
    const auto wait=WaitForSingleObject(proc.hProcess,15000);
    if (wait!=WAIT_OBJECT_0) {
        TerminateProcess(proc.hProcess,99);
        WaitForSingleObject(proc.hProcess,5000);
    }
    DWORD code{};
    GetExitCodeProcess(proc.hProcess,&code);
    CloseHandle(proc.hProcess);
    require(wait==WAIT_OBJECT_0 && code==interrupted,"Worker did not terminate at the requested boundary");
}
void matches(persistence::HistoryStore& store, const domain::Id& id,unsigned char marker) {
    auto lease=store.load(id);
    require(lease.snapshot.formats.size()==1 && lease.snapshot.formats.front().bytes==content(marker).formats.front().bytes,
            "Recovered snapshot does not resolve its original bytes");
}
void crash_case(const fs::path& root,const std::string& boundary) {
    std::string old,floating;
    {
        persistence::HistoryStore store(root);
        store.set_capacity("main",1);
        floating=store.admit("main",content('F'));
        store.detach(floating,{"fixture",-7,48,250,180});
        old=store.admit("main",content('O'));
    }
    launch(root,boundary);
    persistence::HistoryStore recovered(root);
    const auto rope=recovered.hanging("main");
    require(rope.size()==1,"Recovered admission and eviction were split");
    const bool committed=boundary=="committed" || boundary=="gc";
    require((rope[0].id!=old)==committed,"Unexpected recovery side of commit");
    matches(recovered,rope[0].id,committed?'N':'O');
    const auto floats=recovered.floating();
    require(floats.size()==1 && floats[0].id==floating && !floats[0].rope_id &&
            recovered.layout(floating).x==-7,"Independent Floating row or layout lost in recovery");
    matches(recovered,floating,'F');
    require(fs::is_empty(root/"staging"),"Abandoned staging file survived startup GC");
    unsigned files=0;
    for (const auto& item : fs::recursive_directory_iterator(root/"blobs")) if (item.is_regular_file()) ++files;
    require(files==2,"Orphan survived GC or a protected external blob was lost");
    std::cout << "interruption=" << boundary << " committed=" << committed << " healthy_blobs=2\n";
}
void disk_full(const fs::path& root) {
    std::string old;
    {
        persistence::HistoryStore store(root);
        store.set_capacity("main",1);
        old=store.admit("main",content('O'));
        auto* db=store.database().handle();
        persistence::Statement pages(db,"PRAGMA page_count");
        require(pages.step(),"Missing SQLite page count");
        const auto count=pages.integer(0);
        require(!pages.step(),"Unexpected extra page-count result");
        const auto sql="PRAGMA max_page_count="+std::to_string(count);
        persistence::exec(db,sql.c_str());
        domain::Snapshot large;
        for (int i=0;i<64;++i) {
            auto item=content(static_cast<unsigned char>(i),3000).formats[0];
            item.name="Synthetic "+std::to_string(i);
            large.formats.push_back(std::move(item));
        }
        bool full=false;
        try { store.admit("main",large); }
        catch(const std::exception& e) { full=std::string_view(e.what()).find(sqlite3_errstr(SQLITE_FULL))!=std::string_view::npos; }
        require(full,"SQLite did not return SQLITE_FULL at the constrained page limit");
    }
    persistence::HistoryStore reopened(root);
    const auto rows=reopened.hanging("main");
    require(rows.size()==1 && rows[0].id==old,"Disk full lost the previous tail");
    matches(reopened,old,'O');
    std::cout << "SQLite page quota produced SQLITE_FULL; old entry and bytes survived reopen\n";
}
}
int wmain(int argc, wchar_t** argv) {
    try {
        if (argc==4 && std::wstring_view(argv[1])==L"--interrupt") {
            child_crash(argv[2],argv[3]);
        } else if (argc==1) {
            history_fixture::Directory tmp;
            for (const std::string boundary : {"staging","installed","transaction","committed","gc"})
                crash_case(tmp.path/boundary,boundary);
            disk_full(tmp.path/"full");
        } else return 2;
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
