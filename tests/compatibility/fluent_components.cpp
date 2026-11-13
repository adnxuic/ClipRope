#include "platform_win/fluent_tooltip.hpp"
#include "platform_win/menu_bitmap.hpp"
#include <ole2.h>
#include <uiautomation.h>
#include <wrl/client.h>
#include <commctrl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <algorithm>

using namespace cliprope::win;
using namespace cliprope::win::fluent;
namespace {
LRESULT CALLBACK synthetic_pointer(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR) {
    // The private desktop cannot own the physical pointer; departure is sent explicitly.
    if (m==WM_MOUSELEAVE) return 0;
    return DefSubclassProc(h,m,w,l);
}
void check(bool ok,const char* reason) { if (!ok) throw std::runtime_error(reason); }
void pump(unsigned ms) {
    const auto end=GetTickCount64()+ms;
    do {
        MSG m{};
        while (PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
        Sleep(2);
    } while (GetTickCount64()<end);
}
void capture(HWND h,const std::filesystem::path& path) {
    if (path.empty()) return;
    RECT r{}; GetWindowRect(h,&r);
    const int width=r.right-r.left,height=r.bottom-r.top;
    MenuBitmap bitmap;
    check(bitmap.create(width,height),"Capture allocation failed");
    check(PrintWindow(h,bitmap.dc,PW_RENDERFULLCONTENT)!=FALSE,"Capture failed");
    GdiFlush();
    BITMAPINFOHEADER info{sizeof(info),width,-height,1,32,BI_RGB};
    BITMAPFILEHEADER file{0x4d42,DWORD(sizeof(file)+sizeof(info)+width*height*4),0,0,sizeof(file)+sizeof(info)};
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path,std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file),sizeof(file));
    out.write(reinterpret_cast<const char*>(&info),sizeof(info));
    out.write(reinterpret_cast<const char*>(bitmap.pixels),width*height*4);
    check(bool(out),"Capture write failed");
}
void accessible_tooltip(HWND tip) {
    auto result=std::async(std::launch::async,[tip] {
        if (FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED))) return false;
        bool ok{};
        {
            Microsoft::WRL::ComPtr<IUIAutomation> client;
            Microsoft::WRL::ComPtr<IUIAutomationElement> element;
            CONTROLTYPEID type{};
            BSTR name{};
            ok=SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&client))) &&
               SUCCEEDED(client->ElementFromHandle(tip,&element)) &&
               SUCCEEDED(element->get_CurrentControlType(&type)) && type==UIA_ToolTipControlTypeId &&
               SUCCEEDED(element->get_CurrentName(&name)) && name && SysStringLen(name)>0;
            SysFreeString(name);
        }
        CoUninitialize();
        return ok;
    });
    while (result.wait_for(std::chrono::milliseconds(0))!=std::future_status::ready) pump(10);
    check(result.get(),"Tooltip UI Automation name or role missing");
}
void run(const std::filesystem::path& snapshots) {
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"ClipRope.ComponentTest"; check(RegisterClassW(&wc)!=0,"Fixture class failed");
    auto host=CreateWindowExW(0,wc.lpszClassName,L"Fixture",WS_POPUP|WS_VISIBLE|WS_CLIPCHILDREN,
                              20,20,800,700,nullptr,nullptr,wc.hInstance,nullptr);
    check(host!=nullptr,"Fixture window failed");
    Surface surface; surface.attach(host);
    auto target=child(host,L"BUTTON",tr(TextKey::common_save),WS_TABSTOP,10);
    surface.card_style(target,{CardPart::dialog_button,0,tr(TextKey::common_save)});
    MoveWindow(target,10,10,140,36,TRUE);
    auto editor=child(host,L"EDIT",L"Untouched draft",WS_TABSTOP|ES_AUTOHSCROLL,11);
    MoveWindow(editor,10,70,200,30,TRUE); SetFocus(editor); SendMessageW(editor,EM_SETSEL,2,5);
    Tooltip tip;
    const auto first_start=std::chrono::steady_clock::now();
    tip.track(host,target,tr(TextKey::color_default),true);
    UpdateWindow(tip.window());
    const auto first_ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-first_start).count();
    check(tip.visible() && GetFocus()==editor,"Tooltip activated or failed to appear");
    check((GetWindowLongPtrW(tip.window(),GWL_EXSTYLE)&(WS_EX_NOACTIVATE|WS_EX_TRANSPARENT))==
          (WS_EX_NOACTIVATE|WS_EX_TRANSPARENT),"Tooltip input policy changed");
    check(SendMessageW(tip.window(),WM_NCHITTEST,0,0)==HTTRANSPARENT,"Tooltip intercepts input");
    accessible_tooltip(tip.window());
    DWORD start{},end{}; SendMessageW(editor,EM_GETSEL,WPARAM(&start),LPARAM(&end));
    check(start==2 && end==5 && caption(editor)==L"Untouched draft","Tooltip changed edit state");
    if (!snapshots.empty()) capture(tip.window(),snapshots/"tooltip.bmp");
    tip.track(host,target,tr(TextKey::card_editor_plain)+L"\n"+tr(TextKey::card_original_refresh),true,160);
    if (!snapshots.empty()) capture(tip.window(),snapshots/"tooltip-wrapped.bmp");
    SendMessageW(target,WM_MOUSEWHEEL,0,0); check(!tip.visible(),"Wheel did not hide tooltip");
    tip.track(host,target,tr(TextKey::color_default),false);
    SetWindowSubclass(target,synthetic_pointer,1,0);
    SendMessageW(target,WM_MOUSEMOVE,0,MAKELPARAM(5,5));
    check(!tip.visible(),"Hover tooltip ignored dwell");
    pump(280); check(!tip.visible(),"Hover tooltip appeared before 500 ms");
    pump(300);
    check(tip.visible(),"Hover tooltip did not appear after dwell");
    RemoveWindowSubclass(target,synthetic_pointer,1);
    SendMessageW(target,WM_MOUSELEAVE,0,0);
    check(!tip.visible(),"Pointer departure did not hide tooltip");
    tip.hide();
    // Settle the two measured wrapping sizes and native font/frame caches.
    for (int i=0;i<5;++i) { tip.track(host,target,tr(TextKey::color_default),true); UpdateWindow(tip.window()); tip.hide(); }
    pump(50);
    const auto baseline=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    const auto user_baseline=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    const auto targets=Surface::target_creations();
    std::vector<double> times;
    for (int i=0;i<40;++i) {
        const auto opened=std::chrono::steady_clock::now();
        tip.track(host,target,tr(TextKey::color_default),true); UpdateWindow(tip.window());
        times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-opened).count());
        tip.hide();
    }
    pump(100);
    const auto final_gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS);
    const auto final_user=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    std::cout<<"40 tooltip opens: GDI "<<baseline<<" -> "<<final_gdi<<", USER "<<user_baseline<<" -> "<<final_user<<'\n';
    check(final_gdi<=baseline,"Tooltip repeat opens leaked GDI objects");
    check(final_user<=user_baseline,"Tooltip repeat opens leaked USER objects");
    std::sort(times.begin(),times.end());
    std::cout<<"Tooltip first_paint_ms="<<first_ms<<" repeat_median_ms="<<times[20]<<" repeat_p95_ms="<<times[37]
             <<" repeat_target_creations="<<Surface::target_creations()-targets<<" GDI="<<baseline<<" USER="<<user_baseline<<" (stable over 40 opens)\n";
    auto paper=CreateHatchBrush(HS_DIAGCROSS,RGB(215,201,180));
    auto save=child(host,L"BUTTON",tr(TextKey::common_save),WS_TABSTOP,12);
    surface.paper_style(save,Icon::check,[&] { return PaperInk{paper,RGB(251,247,239),RGB(50,45,40)}; });
    MoveWindow(save,10,130,px(host,20),px(host,20),TRUE);
    pump(30);
    {
        RECT bounds{}; GetClientRect(save,&bounds);
        MenuBitmap pixels; check(pixels.create(bounds.right,bounds.bottom),"Paper icon fixture allocation failed");
        check(PrintWindow(save,pixels.dc,PW_RENDERFULLCONTENT)!=FALSE,"Paper icon capture failed"); GdiFlush();
        unsigned strokes{};
        for (int y=px(host,4);y<bounds.bottom-px(host,4);++y)
            for (int x=px(host,4);x<bounds.right-px(host,4);++x) {
                const auto c=pixels.pixels[y*bounds.right+x];
                const unsigned r=(c>>16)&255,g=(c>>8)&255,b=c&255;
                strokes += surface.colors.contrast ? r>200 && g>200 && b>200 : r<100 && g<100 && b<100;
            }
        check(strokes>2,"Save icon has no visible strokes over the paper texture");
    }
    if (!snapshots.empty()) capture(save,snapshots/"paper-save.bmp");
    if (!snapshots.empty()) {
        auto state_frames=[&](HWND action,const std::wstring& prefix) {
            auto shot=[&](const wchar_t* state) { pump(125); capture(action,snapshots/(prefix+state+L".bmp")); };
            SetFocus(editor); shot(L"-normal");
            SetFocus(action); SendMessageW(action,WM_UPDATEUISTATE,MAKEWPARAM(UIS_CLEAR,UISF_HIDEFOCUS),0); shot(L"-focus");
            SetFocus(editor); SetWindowSubclass(action,synthetic_pointer,1,0);
            SendMessageW(action,WM_MOUSEMOVE,0,MAKELPARAM(5,5)); shot(L"-hover");
            SendMessageW(action,BM_SETSTATE,TRUE,0); shot(L"-pressed");
            SendMessageW(action,BM_SETSTATE,FALSE,0); EnableWindow(action,FALSE); shot(L"-disabled");
            RemoveWindowSubclass(action,synthetic_pointer,1); SendMessageW(action,WM_MOUSELEAVE,0,0); EnableWindow(action,TRUE);
        };
        state_frames(save,L"paper");
        auto cancel=child(host,L"BUTTON",tr(TextKey::common_cancel),WS_TABSTOP,13);
        surface.paper_style(cancel,Icon::close,[&] { return PaperInk{paper,RGB(251,247,239),RGB(50,45,40)}; });
        MoveWindow(cancel,70,130,px(host,20),px(host,20),TRUE);
        state_frames(cancel,L"cancel");
        auto color=child(host,L"BUTTON",tr(TextKey::rope_color_label),WS_TABSTOP,14);
        surface.card_style(color,{CardPart::color_picker,0,L"#C9A26B",{},{},RGB(201,162,107)});
        MoveWindow(color,10,250,px(host,150),px(host,36),TRUE);
        state_frames(color,L"color");
    }
    Surface::discard_target(); InvalidateRect(save,nullptr,FALSE); pump(30);
    check(caption(editor)==L"Untouched draft","Renderer recovery changed edit state");
    tip.track(host,target,tr(TextKey::color_default),false);
    SendMessageW(target,WM_MOUSEMOVE,0,MAKELPARAM(5,5));
    DestroyWindow(target); pump(550); check(!tip.visible(),"Destroyed tooltip target remained visible");
    tip.track(host,save,tr(TextKey::color_default),true);
    check(tip.visible(),"Immediate target destruction fixture did not show");
    DestroyWindow(save); check(!tip.visible(),"Destroyed visible target left a tooltip");
    const RECT hit_region{210,170,230,190};
    tip.track(host,host,tr(TextKey::color_default),false,160,&hit_region);
    SetWindowSubclass(host,synthetic_pointer,1,0);
    SendMessageW(host,WM_MOUSEMOVE,0,MAKELPARAM(220,180)); pump(550);
    check(tip.visible(),"Hit-region tooltip did not appear");
    RECT tip_rect{},owner_rect{}; GetWindowRect(tip.window(),&tip_rect); GetWindowRect(host,&owner_rect);
    check(tip_rect.left==owner_rect.left+hit_region.left && tip_rect.top>=owner_rect.top+hit_region.bottom &&
          tip_rect.top<owner_rect.top+hit_region.bottom+px(host,20),"Hit-region tooltip anchored to the whole owner");
    SendMessageW(host,WM_MOUSEMOVE,0,MAKELPARAM(240,200));
    check(!tip.visible(),"Leaving the hit region did not hide the tooltip");
    RemoveWindowSubclass(host,synthetic_pointer,1);
    tip.clear(); DestroyWindow(host); DeleteObject(paper);
}
}
int wmain(int argc,wchar_t** argv) {
    cliprope::localization::LocalizationManager manager;
    manager.set_language(cliprope::localization::Language::automatic,preferred_ui_locale());
    UiLocalization language(manager);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto desktop=CreateDesktopW((L"ClipRope.Components."+std::to_wstring(GetCurrentProcessId())).c_str(),
                                nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if (!desktop || !SetThreadDesktop(desktop) || FAILED(OleInitialize(nullptr))) return 2;
    int code{};
    try { run(argc>1 ? std::filesystem::path(argv[1]) : std::filesystem::path{}); }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; code=1; }
    OleUninitialize(); return code;
}
