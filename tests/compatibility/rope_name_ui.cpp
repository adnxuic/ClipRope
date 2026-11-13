#include "../integration/history_fixture.hpp"
#include "management_frame.hpp"
#include "persistence/browse.hpp"
#include "persistence/history_store.hpp"
#include "platform_win/rope_manager.hpp"
#include "platform_win/rope_name.hpp"
#include <iostream>

namespace {
using namespace cliprope;
using history_fixture::require;
namespace fs = std::filesystem;

void replace(HWND field, const std::wstring& text) {
    SendMessageW(field,EM_SETSEL,0,-1);
    SendMessageW(field,EM_REPLACESEL,TRUE,LPARAM(text.c_str()));
    require(win::fluent::caption(field)==text,"Native field truncated the proposed name before validation");
}

void exercise(const fs::path& root, const fs::path& output, bool chinese) {
    localization::LocalizationManager language;
    language.set_language(chinese ? localization::Language::simplified_chinese : localization::Language::english);
    win::UiLocalization binding(language);
    persistence::HistoryStore store(root);
    persistence::Browser records(store);
    const auto id=store.create_rope("Ordinary",5);
    const auto locale=chinese ? "-zh.bmp" : "-en.bmp";
    const auto error=std::wstring(language.tr(localization::TextKey::error_rope_name));
    const auto valid=std::wstring(86,L'汉');
    win::RopeActions actions;
    actions.default_capacity=[] {return 5;};
    actions.current=[&](const auto& rope){return records.rope(rope);};
    actions.preview=[&](const auto& change){return store.preview_rope_change(change);};
    actions.apply=[&](const auto& preview){return store.apply_rope_change(preview);};
    actions.select=[&](auto rope){store.set_active_rope(rope);};
    win::RopeManager manager(std::move(actions));
    const auto owner=CreateWindowW(L"STATIC",L"Rope fixture",WS_POPUP,20,20,50,50,nullptr,nullptr,nullptr,nullptr);
    require(owner!=nullptr,"Cannot create private owner");
    struct Owner { HWND w; ~Owner(){DestroyWindow(w);} } cleanup{owner};
    manager.open(owner,records.rope(id),application::RopeOp::create);
    auto dialog=FindWindowW(L"ClipRope.RopeManager",nullptr);
    require(dialog && GetDlgItem(dialog,1901),"Missing create dialog");
    replace(GetDlgItem(dialog,1901),std::wstring(257,L'汉'));
    SendMessageW(dialog,WM_COMMAND,IDOK,0);
    require(win::fluent::caption(GetDlgItem(dialog,1905))==error,"Create did not explain its name rejection");
    management_fixture::frame(dialog,output/(std::string("dialog-error")+locale));
    replace(GetDlgItem(dialog,1901),valid);
    SendMessageW(dialog,WM_COMMAND,IDOK,0);
    auto confirm=FindWindowW(L"ClipRope.RopeManager",nullptr);
    require(confirm && !GetDlgItem(confirm,1901),"86 Han characters were rejected at create");
    SendMessageW(confirm,WM_COMMAND,IDCANCEL,0);
    require(records.rope_count()==3,"Create did not commit exactly once");

    struct Popup {
        persistence::Browser& records;
        const domain::Id& id;
        const fs::path& output;
        const char* locale;
        const std::wstring& error;
        unsigned step{};
        std::exception_ptr failure;
    } run{records,id,output,locale,error};
    SetWindowLongPtrW(owner,GWLP_USERDATA,LONG_PTR(&run));
    require(SetTimer(owner,1,30,[](HWND owner,UINT,UINT_PTR,DWORD){
        const auto menu=FindWindowW(L"ClipRope.RopeMenu",nullptr);
        const auto field=GetDlgItem(menu,11901);
        if(!field) return;
        auto& run=*reinterpret_cast<Popup*>(GetWindowLongPtrW(owner,GWLP_USERDATA));
        try {
            if(run.step==0) {
                management_fixture::frame(menu,run.output/(std::string("popup-normal")+run.locale));
                replace(field,std::wstring(257,L'Z'));
            } else if(run.step==1) {
                require(win::fluent::caption(GetDlgItem(menu,9000)).find(run.error)!=std::wstring::npos &&
                        run.records.rope(run.id).name=="Ordinary","Popup omitted the reason or committed an invalid name");
                management_fixture::frame(menu,run.output/(std::string("popup-error")+run.locale));
                replace(field,std::wstring(256,L'汉'));
            } else {
                require(run.records.rope(run.id).name==win::unicode::utf8(std::wstring(256,L'汉')),
                        "Popup rejected its maximum Unicode name");
            }
            if(run.step++<2) {
                SetFocus(field);
                PostMessageW(field,WM_KEYDOWN,VK_RETURN,0);
                return;
            }
        } catch(...) {run.failure=std::current_exception();}
        KillTimer(owner,1);
        PostMessageW(menu,WM_KEYDOWN,VK_ESCAPE,0);
    }), "Cannot drive native property popup");
    manager.menu(owner,{70,70},records.rope(id),{false,false,false,false,true});
    KillTimer(owner,1);
    if(run.failure) std::rethrow_exception(run.failure);
    require(run.step==3,"Native popup did not complete");

    const auto saved = records.rope(id).name;
    manager.open(owner,records.rope(id),application::RopeOp::properties);
    dialog=FindWindowW(L"ClipRope.RopeManager",nullptr);
    require(win::fluent::caption(GetDlgItem(dialog,1901))==win::unicode::wide(saved),"Saved Unicode name changed");
    management_fixture::frame(dialog,output/(std::string("saved")+locale));
    replace(GetDlgItem(dialog,1902),L"6");
    SendMessageW(dialog,WM_COMMAND,IDOK,0);
    require(records.rope(id).name==saved && records.rope(id).capacity==6,"Capacity edit changed the saved name");
}
}
int wmain(int argc,wchar_t** argv) {
    if(argc!=2) return 2;
    const auto previous=GetThreadDesktop(GetCurrentThreadId());
    const auto name=L"ClipRope.RopeNames."+std::to_wstring(GetCurrentProcessId());
    const auto desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if(!desktop || !SetThreadDesktop(desktop)) return 2;
    struct Desktop {HDESK previous,desk; ~Desktop(){SetThreadDesktop(previous); CloseDesktop(desk);}} isolated{previous,desktop};
    management_fixture::record_frames=true;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        fs::create_directories(argv[1]);
        history_fixture::Directory tmp;
        exercise(tmp.path/"en",argv[1],false);
        exercise(tmp.path/"zh",argv[1],true);
        return 0;
    } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
