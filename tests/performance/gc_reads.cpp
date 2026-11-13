#include "../integration/resident.hpp"
#include "../integration/history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "persistence/browse.hpp"
#include "platform_win/performance_counters.hpp"
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>

namespace {
using namespace cliprope;
using probe::require;
using Clock=std::chrono::steady_clock;
namespace fs=std::filesystem;
double ms(Clock::time_point start) { return std::chrono::duration<double,std::milli>(Clock::now()-start).count(); }
domain::Snapshot content() { return {{{CF_UNICODETEXT,{}, {'A',0,0,0}}}}; }
void exercise(const wchar_t* exe, unsigned trial, bool slow) {
    history_fixture::Directory dir;
    const auto profile=dir.path/"profile";
    bool measuring{};
    std::promise<void> scan;
    Clock::time_point sql, enumeration, deletion;
    double sql_ms{}, scan_ms{}, delete_ms{};
    domain::Id captured;
    {
        persistence::HistoryStore store(profile,[&](std::string_view point) {
            if (!measuring) return;
            if (point=="gc-sql-start") sql=Clock::now();
            else if (point=="gc-scan-start") {
                sql_ms=ms(sql);
                enumeration=Clock::now();
                scan.set_value();
                if (slow) Sleep(150);
            } else if (point=="gc-scan-done") scan_ms=ms(enumeration);
            else if (point=="gc-delete-start") deletion=Clock::now();
            else if (point=="gc-delete-done") delete_ms+=ms(deletion);
        });
        store.set_capacity("main",1024);
        for (unsigned i=0;i<256;++i) store.admit("main",content());
        const auto thumbs=profile/"thumbnails";
        fs::create_directories(thumbs);
        for (unsigned i=0;i<512;++i) {
            std::ofstream foreign(thumbs/(std::to_string(i)+".foreign"));
            foreign.put('f');
        }
        const auto locked=profile/"staging"/"0123456789abcdef0123456789abcdef.part";
        { std::ofstream file(locked); file.put('x'); }
        const auto native=CreateFileW(locked.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
        require(native!=INVALID_HANDLE_VALUE,"Cannot hold orphan");
        std::unique_ptr<void,decltype(&CloseHandle)> held(native,CloseHandle);
        measuring=true;
        auto collecting=std::async(std::launch::async,[&]{return store.gc();});
        scan.get_future().get();
        auto admitting=std::async(std::launch::async,[&]{return store.admit_capture("main",content());});
        auto backup=std::async(std::launch::async,[&]{store.database().backup_to(dir.path/"backup");});
        auto t=Clock::now();
        { std::lock_guard guard(store.database().mutex()); }
        const auto mutex_ms=ms(t);
        t=Clock::now();
        persistence::Browser reader(store);
        require(!reader.page("main",0,12).empty(),"Foreground read lost the rope");
        const auto page_ms=ms(t);
        auto result=collecting.get();
        captured=admitting.get();
        backup.get();
        measuring=false;
        require(result.deferred>=1 && fs::exists(locked) && fs::exists(thumbs/"3.foreign"),"Collector damaged a foreign or locked file");
        held.reset(); store.gc();
        require(!fs::exists(locked) && store.load(captured).snapshot.formats.front().bytes==content().formats.front().bytes,
                "Released orphan or racing capture incorrect");
        persistence::Database::verify_store(dir.path/"backup");
        std::cout<<"gc trial="<<trial<<" slow="<<slow<<" primary_wait_ms="<<mutex_ms<<" page_ms="<<page_ms
                 <<" sql_reach_commit_ms="<<sql_ms<<" scan_ms="<<scan_ms<<" delete_ms="<<delete_ms
                 <<" deferred="<<result.deferred<<'\n';
    }
    const auto startup=Clock::now();
    probe::Resident host(exe,profile,true,dir.path/"log.txt",L"gc-probe");
    const auto ready=ms(startup);
    const auto name=L"Local\\ClipRope.GCPage."+std::to_wstring(host.pid());
    std::unique_ptr<void,decltype(&CloseHandle)> event(CreateEventW(nullptr,TRUE,FALSE,name.c_str()),CloseHandle);
    require(event && PostMessageW(host.listener(),win::gc_probe_start,0,0),"Cannot start desktop collection");
    require(WaitForSingleObject(event.get(),3000)==WAIT_OBJECT_0,"Desktop did not enter page read");
    const auto beat=Clock::now();
    DWORD_PTR ignored{};
    require(SendMessageTimeoutW(host.listener(),WM_NULL,0,0,SMTO_ABORTIFHUNG,2500,&ignored)!=0,"Desktop heartbeat failed");
    const auto latency=ms(beat);
    host.exit();
    require(latency<130.0,"Foreground page waited for the held filesystem scan");
    std::cout<<"gc_host trial="<<trial<<" slow="<<slow<<" startup_ms="<<ready<<" held_page_heartbeat_ms="<<latency<<'\n';
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2 || !probe::private_station()) return 2;
    try {
        for(unsigned i=1;i<=3;++i) for(bool slow:{false,true}) exercise(argv[1],i,slow);
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
}
