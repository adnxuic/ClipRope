#include "platform_win/uninstall_dialog.hpp"
#include "platform_win/fluent.hpp"
#include "platform_win/menu_bitmap.hpp"
#include <ole2.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <vector>

using namespace cliprope;
using namespace cliprope::win;
using namespace cliprope::win::fluent;
namespace fs=std::filesystem;
namespace {
void check(bool value,const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void await(F predicate,const char* reason) {
    const auto end=GetTickCount64()+7000;
    do { if (predicate()) return; Sleep(5); } while (GetTickCount64()<end);
    throw std::runtime_error(reason);
}
std::wstring caption_of(HWND h) {
    const auto length=GetWindowTextLengthW(h);
    std::wstring value(size_t(length)+1,L'\0');
    value.resize(GetWindowTextW(h,value.data(),int(value.size()))); return value;
}
HWND dialog_for(DWORD thread) {
    HWND found{};
    EnumThreadWindows(thread,[](HWND h,LPARAM value)->BOOL {
        if (GetDlgItem(h,uninstall_history_id) && IsWindowVisible(h)) {
            *reinterpret_cast<HWND*>(value)=h; return FALSE;
        }
        return TRUE;
    },reinterpret_cast<LPARAM>(&found));
    return found;
}
void capture(HWND h,const fs::path& path) {
    if (path.empty()) return;
    RECT r{}; GetWindowRect(h,&r);
    const auto width=r.right-r.left,height=r.bottom-r.top;
    MenuBitmap bitmap; check(bitmap.create(width,height),"Capture allocation failed");
    check(PrintWindow(h,bitmap.dc,PW_RENDERFULLCONTENT)!=FALSE,"Capture failed");
    GdiFlush();
    BITMAPINFOHEADER info{sizeof(info),width,-height,1,32,BI_RGB};
    BITMAPFILEHEADER file{0x4d42,DWORD(sizeof(file)+sizeof(info)+width*height*4),0,0,sizeof(file)+sizeof(info)};
    fs::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file),sizeof(file));
    out.write(reinterpret_cast<const char*>(&info),sizeof(info));
    out.write(reinterpret_cast<const char*>(bitmap.pixels),width*height*4);
    check(bool(out),"Capture write failed");
}
void toggle_provider(HWND control,const std::wstring& expected,bool checked) {
    Microsoft::WRL::ComPtr<IUIAutomation> client;
    Microsoft::WRL::ComPtr<IUIAutomationElement> element;
    Microsoft::WRL::ComPtr<IUIAutomationTogglePattern> toggle;
    check(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&client))) &&
          SUCCEEDED(client->ElementFromHandle(control,&element)) &&
          SUCCEEDED(element->GetCurrentPatternAs(UIA_TogglePatternId,IID_PPV_ARGS(&toggle))),"Native history Toggle provider unavailable");
    BSTR name{}; check(SUCCEEDED(element->get_CurrentName(&name)),"History accessible name unavailable");
    const bool named=name && expected==name; SysFreeString(name); check(named,"History accessible name changed");
    ToggleState state{}; check(SUCCEEDED(toggle->get_CurrentToggleState(&state)) &&
        state==(checked ? ToggleState_On : ToggleState_Off),"Native Toggle state disagreed with the control");
}
double scene(std::string_view mode,const fs::path& output) {
    const DWORD ui_thread=GetCurrentThreadId();
    const auto desktop=GetThreadDesktop(ui_thread);
    const auto expected_name=std::wstring(tr(TextKey::uninstall_history).view());
    std::wstring directory=L"E:\\ClipRope UI fixture\\Release";
    if (mode=="long") for (int i=0;i<20;++i) directory+=L"\\long-directory-\u76ee\u5f55";
    const bool foreign=mode=="foreign",invalid=mode=="invalid";
    std::atomic<unsigned> calls{};
    std::atomic<bool> erased{};
    std::string failure;
    double opened{};
    std::chrono::steady_clock::time_point started;
    std::promise<void> prepared;
    auto preparation=prepared.get_future();
    std::jthread controller([&] {
        HWND h{};
        bool initialized{},ready{};
        try {
            check(SetThreadDesktop(desktop)!=FALSE,"Controller desktop unavailable");
            check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"Controller COM failed");
            initialized=true;
            prepared.set_value(); ready=true;
            await([&] { h=dialog_for(ui_thread); return h!=nullptr; },"Uninstall dialog did not open");
            GUITHREADINFO focus{sizeof(focus)};
            await([&] { return GetGUIThreadInfo(ui_thread,&focus) && focus.hwndFocus==GetDlgItem(h,IDCANCEL); },"Cancel was not initially focused");
            RedrawWindow(h,nullptr,nullptr,RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            opened=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();
            const auto history=GetDlgItem(h,uninstall_history_id);
            check(SendMessageW(history,BM_GETCHECK,0,0)==BST_UNCHECKED,"Destructive history choice defaulted on");
            check(bool(IsWindowEnabled(history))==!(foreign || invalid),"History availability disagreed with ownership");
            RECT status{},action{}; GetWindowRect(GetDlgItem(h,uninstall_status_id),&status); GetWindowRect(GetDlgItem(h,IDOK),&action);
            check(status.bottom<=action.top,"Uninstall status overlaps the actions");
            toggle_provider(history,expected_name,false);
            if (!output.empty()) capture(h,output/(std::string(mode)+".bmp"));
            if (mode=="keyboard") {
                auto focused=[&] { GUITHREADINFO info{sizeof(info)}; return GetGUIThreadInfo(ui_thread,&info) ? info.hwndFocus : nullptr; };
                for (int step=0;step<5 && focused()!=history;++step) {
                    const auto previous=focused();
                    PostMessageW(previous,WM_KEYDOWN,VK_TAB,1);
                    await([&] { return focused()!=previous; },"Tab did not advance focus");
                }
                check(focused()==history,"Keyboard could not reach the history choice");
                for (const auto expected:{BST_CHECKED,BST_UNCHECKED}) {
                    PostMessageW(history,WM_KEYDOWN,VK_SPACE,1);
                    PostMessageW(history,WM_KEYUP,VK_SPACE,LPARAM(0xc0000001u));
                    await([&] { return SendMessageW(history,BM_GETCHECK,0,0)==expected; },"Space did not toggle history");
                    toggle_provider(history,expected_name,expected==BST_CHECKED);
                }
                SendMessageW(h,WM_NEXTDLGCTL,WPARAM(GetDlgItem(h,IDCANCEL)),TRUE);
                PostMessageW(GetDlgItem(h,IDCANCEL),WM_KEYDOWN,VK_RETURN,1);
            } else if (mode=="cancel" || foreign || invalid || mode=="long") {
                PostMessageW(h,WM_KEYDOWN,VK_ESCAPE,0);
            } else {
                if (mode=="delete" || mode=="retry") {
                    SendMessageW(history,BM_CLICK,0,0);
                    check(SendMessageW(history,BM_GETCHECK,0,0)==BST_CHECKED,"History switch did not toggle");
                    toggle_provider(history,expected_name,true);
                    if (!output.empty()) capture(h,output/(std::string(mode)+"-checked.bmp"));
                }
                SendMessageW(GetDlgItem(h,IDOK),BM_CLICK,0,0);
                await([&] { return !IsWindowEnabled(GetDlgItem(h,IDOK)); },"Pending action stayed enabled");
                PostMessageW(h,WM_CLOSE,0,0); Sleep(10);
                check(IsWindow(h),"Pending close invalidated the operation owner");
                if (!output.empty()) capture(h,output/(std::string(mode)+"-pending.bmp"));
                if (mode=="retry") {
                    await([&] { return IsWindowEnabled(GetDlgItem(h,IDOK)) && IsWindowVisible(GetDlgItem(h,IDCANCEL)); },"Retry feedback did not recover input");
                    check(SendMessageW(history,BM_GETCHECK,0,0)==BST_CHECKED,"Failed operation discarded the history choice");
                    check(caption_of(GetDlgItem(h,uninstall_status_id)).find(L"ClipRope.exe")!=std::wstring::npos,"Failure did not identify the blocked runtime");
                    if (!output.empty()) capture(h,output/"retry-error.bmp");
                    SendMessageW(GetDlgItem(h,IDOK),BM_CLICK,0,0);
                }
                await([&] { return IsWindow(h) && !IsWindowVisible(GetDlgItem(h,IDCANCEL)); },"Successful operation did not present completion");
                check(!IsWindowEnabled(history),"Completed history choice stayed interactive");
                if (!output.empty()) capture(h,output/(std::string(mode)+"-done.bmp"));
                if (mode=="delete") PostMessageW(h,WM_KEYDOWN,VK_ESCAPE,0);
                else SendMessageW(GetDlgItem(h,IDOK),BM_CLICK,0,0);
            }
        } catch (const std::exception& error) {
            if (!ready) prepared.set_exception(std::current_exception());
            failure=error.what();
            for (int i=0;i<100 && IsWindow(h);++i) { PostMessageW(h,WM_CLOSE,0,0); Sleep(20); }
        }
        if (initialized) CoUninitialize();
    });
    preparation.get();
    started=std::chrono::steady_clock::now();
    const bool done=uninstall_dialog(nullptr,{
        [=] {
            BuildUninstallPreview info{fs::path(directory),!(foreign || invalid),foreign};
            if (invalid) info.issue=UninstallIssue::layout;
            return info;
        },
        [&](bool purge) {
            const auto attempt=++calls; erased=purge;
            Sleep(350);
            if (mode=="retry" && attempt==1)
                return BuildUninstallResult{false,false,UninstallIssue::payload,L"ClipRope.exe",ERROR_SHARING_VIOLATION};
            return BuildUninstallResult{true,purge};
        }});
    controller.join(); check(failure.empty(),failure.c_str());
    const bool expected_done=mode=="normal" || mode=="delete" || mode=="retry";
    check(done==expected_done,"Dialog completion/cancellation result was wrong");
    check(calls==(mode=="retry" ? 2u : expected_done ? 1u : 0u),"Dialog dispatched an unexpected number of cleanup commands");
    if (expected_done) check(erased==(mode=="delete" || mode=="retry"),"Dialog changed the explicit history choice");
    return opened;
}
} // namespace
int wmain(int argc,wchar_t** argv) {
    const auto desktop=CreateDesktopW((L"ClipRope.UninstallUI."+std::to_wstring(GetCurrentProcessId())).c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if (!desktop || !SetThreadDesktop(desktop) || FAILED(OleInitialize(nullptr))) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int code{};
    try {
        check(argc==1 || argc==2,"Pass an optional capture directory");
        const bool keyboard=argc==2 && std::wstring_view(argv[1])==L"--keyboard";
        const fs::path out=argc==2 && !keyboard ? fs::path(argv[1]) : fs::path{};
        localization::LocalizationManager manager;
        UiLocalization language(manager);
        for (const auto [locale,name]:{std::pair{localization::Language::english,"en-US"},
                                       std::pair{localization::Language::simplified_chinese,"zh-CN"}}) {
            manager.set_language(locale,preferred_ui_locale());
            if (keyboard) { scene("keyboard",{}); continue; }
            for (auto mode:{"normal","delete","retry","foreign","invalid","long","cancel"}) {
                const auto elapsed=scene(mode,out.empty() ? fs::path{} : out/name);
                std::cout<<name<<' '<<mode<<" open/paint_ms="<<elapsed<<'\n';
            }
        }
        if (keyboard) std::cout<<"Native Tab traversal, Space Toggle/UIA and Enter cancellation passed in both languages\n";
        else if (out.empty()) {
        scene("cancel",{}); scene("cancel",{});
        const auto process=GetCurrentProcess();
        const auto gdi=GetGuiResources(process,GR_GDIOBJECTS),user=GetGuiResources(process,GR_USEROBJECTS);
        const auto targets=Surface::target_creations();
        std::vector<double> warm;
        for (int repeat=0;repeat<10;++repeat) warm.push_back(scene("cancel",{}));
        check(GetGuiResources(process,GR_GDIOBJECTS)<=gdi && GetGuiResources(process,GR_USEROBJECTS)<=user,"Repeated close/open leaked native UI resources");
        check(Surface::target_creations()==targets,"Repeated dialogs created renderer targets");
        std::cout<<"10 warm opens: GDI "<<gdi<<" -> "<<GetGuiResources(process,GR_GDIOBJECTS)<<" USER "<<user<<" -> "<<GetGuiResources(process,GR_USEROBJECTS)<<" targets=0; ms";
        for (auto value:warm) std::cout<<' '<<value;
        std::cout<<"\nNative Toggle/name, default cancellation, pending close, keep/delete, retry and lifetime checks passed\n";
        }
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; code=1; }
    OleUninitialize(); return code;
}
