#include "../integration/resident.hpp"
#include "../integration/history_fixture.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "platform_win/performance_counters.hpp"
#include "platform_win/file_group.hpp"
#include "persistence/history_store.hpp"
#include "process_metrics.hpp"
#include <dbt.h>
#include "application/preview_changes.hpp"
#include <chrono>
#include <fstream>
#include <iostream>

namespace {
using namespace cliprope;
using probe::require;
using win::PerformanceCounter;
namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;
DWORD_PTR query(HWND window,UINT message,WPARAM w=0,LPARAM l=0) {
    DWORD_PTR value{};
    require(SendMessageTimeoutW(window,message,w,l,SMTO_ABORTIFHUNG,2500,&value)!=0,"Population query timed out");
    return value;
}
void measure(const wchar_t* exe,unsigned n,bool visible,unsigned trial) {
    history_fixture::Directory dir;
    auto file=dir.path/"image.png";
    const auto png=media_sample::png();
    { std::ofstream out(file,std::ios::binary); out.write(reinterpret_cast<const char*>(png.bytes.data()),png.bytes.size()); }
    domain::Snapshot image{{{CF_HDROP,{},encode_file_group({file.wstring()})}}},text{{{CF_UNICODETEXT,{}, {'X',0,0,0}}}};
    image.path_kind=domain::PathKind::file;
    const auto shared=std::min(n,2u);
    {
        persistence::HistoryStore store(dir.path);
        store.admit("main",image);
        for(unsigned i=0;i<n;++i) {
            auto id=store.admit("main",i<shared ? image : text);
            store.detach(id,{"",55.+10*(i%8),65.+10*(i%7),240,160,visible});
        }
    }
    const auto start=Clock::now();
    probe::Resident host(exe,dir.path,visible);
    auto ready=std::chrono::duration<double,std::milli>(Clock::now()-start).count();
    const auto metric=[&](PerformanceCounter c){return query(host.listener(),win::performance_query,WPARAM(c));};
    auto resources=performance::process_tree(host.pid());
    unsigned hwnds{};
    for(HWND h=nullptr;(h=FindWindowExW(nullptr,h,L"ClipRope.Floating",nullptr))!=nullptr;) {
        DWORD owner{}; GetWindowThreadProcessId(h,&owner); if(owner==host.pid()) ++hwnds;
    }
    std::cout<<"population trial="<<trial<<" n="<<n<<" visible="<<visible<<" ready_ms="<<ready<<" hwnds="<<hwnds
             <<" group_private="<<resources.total_private()<<" handles="<<resources.handles<<" gdi="<<resources.gdi<<" user="<<resources.user;
    if(visible) {
        probe::pump_until([&]{return metric(PerformanceCounter::visible_file_consumers)==shared;},
                          "Shared Floating consumers did not receive the image");
        Sleep(100);
        const auto reads=metric(PerformanceCounter::floating_summary_reads), notices=metric(PerformanceCounter::preview_notices);
        const auto batches=metric(PerformanceCounter::preview_batches), floats=metric(PerformanceCounter::floating_frames);
        const auto workspace=metric(PerformanceCounter::painted_frames), decodes=metric(PerformanceCounter::preview_decodes);
        fs::last_write_time(file,fs::last_write_time(file)+std::chrono::seconds(2));
        WIN32_FILE_ATTRIBUTE_DATA info{};
        require(GetFileAttributesExW(file.c_str(),GetFileExInfoStandard,&info)!=FALSE,"File metadata unavailable");
        const auto version = ((std::uint64_t(info.ftLastWriteTime.dwHighDateTime)<<32)|info.ftLastWriteTime.dwLowDateTime) ^
                             ((std::uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow);
        query(host.listener(),WM_DEVICECHANGE,DBT_DEVNODES_CHANGED);
        probe::pump_until([&]{ return metric(PerformanceCounter::preview_decodes)>decodes &&
            metric(PerformanceCounter::preview_notices)>notices &&
            query(host.listener(),win::performance_query,WPARAM(PerformanceCounter::visible_file_consumers),LPARAM(version))==shared; },
            "Shared refresh did not settle");
        Sleep(120);
        require(metric(PerformanceCounter::floating_summary_reads)-reads <= n + shared*(metric(PerformanceCounter::preview_notices)-notices),
                "A dirty key refreshed unrelated Floating cards");
        std::cout<<" file_keys=1 consumers="<<shared<<" notices="<<metric(PerformanceCounter::preview_notices)-notices
                 <<" batches="<<metric(PerformanceCounter::preview_batches)-batches
                 <<" float_reads="<<metric(PerformanceCounter::floating_summary_reads)-reads
                 <<" float_frames="<<metric(PerformanceCounter::floating_frames)-floats
                 <<" workspace_frames="<<metric(PerformanceCounter::painted_frames)-workspace;
    }
    host.exit();
    persistence::HistoryStore reopened(dir.path);
    const auto entries=reopened.floating();
    require(entries.size()==n,"Restart lost Floating membership");
    for(const auto& e:entries) require(reopened.layout(e.id).visible==visible,"Restart changed visibility");
    std::cout<<'\n';
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2 || !probe::private_station()) return 2;
    try {
        application::PreviewChanges dirty;
        dirty.add("one",1);
        dirty.add("one",1);
        require(!dirty.all && dirty.blobs.size()==1 && dirty.contains("one"),"Duplicate keys did not coalesce");
        dirty.add("two",2);
        require(dirty.all && dirty.contains("unrelated"),"Mixed generations lost full refresh");
        dirty={};
        for(unsigned i=0;i<65;++i) dirty.add(std::to_string(i),3);
        require(dirty.all && dirty.blobs.empty(),"Saturated changes did not fall back");
        dirty={};
        dirty.add({},1);
        require(dirty.all,"Unknown identity did not fall back");
        for(unsigned trial=1;trial<=3;++trial) {
            for(unsigned n:{1u,24u,64u}) measure(argv[1],n,true,trial);
            for(unsigned n:{24u,64u}) measure(argv[1],n,false,trial);
        }
        return 0;
    } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
