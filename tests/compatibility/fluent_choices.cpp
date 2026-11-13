#include "platform_win/fluent_list.hpp"
#include "platform_win/card_menu_frame.hpp"
#include "platform_win/menu_bitmap.hpp"
#include <commctrl.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace cliprope::win;
using namespace cliprope::win::fluent;
namespace {
void check(bool ok,const char* reason) { if (!ok) throw std::runtime_error(reason); }
void drain() {
    MSG m{};
    while (PeekMessageW(&m,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); }
}
struct Observation {
    COLORREF panel;
    bool in_region{}, valid_margin{true};
    unsigned nested{}, snapshots{};
};
LRESULT CALLBACK observe(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
    auto& o=*reinterpret_cast<Observation*>(data);
    const bool previous=o.in_region;
    if (m==WM_WINDOWPOSCHANGED) { if (previous) ++o.nested; o.in_region=true; }
    const auto result=DefSubclassProc(h,m,w,l);
    if (m==WM_WINDOWPOSCHANGED) o.in_region=previous;
    if (m==WM_PRINT && (l & PRF_NONCLIENT)) {
        RECT r{}; GetWindowRect(h,&r); GdiFlush();
        // Inspect the actual DC requested by native expansion, not another painter.
        o.valid_margin &= GetPixel(HDC(w),(r.right-r.left)/2,px(h,2))==o.panel;
        ++o.snapshots;
    }
    return result;
}
HWND frame_for(HWND choice) {
    struct Search { HWND owner, result{}; } search{GetAncestor(choice,GA_ROOT)};
    EnumThreadWindows(GetCurrentThreadId(),[](HWND h,LPARAM data) -> BOOL {
        auto& s=*reinterpret_cast<Search*>(data);
        wchar_t name[80]{}; GetClassNameW(h,name,80);
        if (GetWindow(h,GW_OWNER)==s.owner && wcscmp(name,L"ClipRope.CardMenuFrame")==0) s.result=h;
        return TRUE;
    },LPARAM(&search));
    return search.result;
}
void snapshot(HWND list,const std::filesystem::path& file) {
    RECT r{}; GetWindowRect(list,&r);
    const int width=r.right-r.left,height=r.bottom-r.top;
    MenuBitmap bitmap; check(bitmap.create(width,height),"Snapshot allocation failed");
    const RECT image{0,0,width,height};
    solid(bitmap.dc,image,RGB(255,0,255));
    SendMessageW(list,WM_PRINT,WPARAM(bitmap.dc),PRF_CLIENT|PRF_NONCLIENT|PRF_ERASEBKGND);
    if (file.empty()) return;
    GdiFlush();
    BITMAPINFOHEADER info{sizeof(info),width,-height,1,32,BI_RGB};
    BITMAPFILEHEADER header{0x4d42,DWORD(sizeof(header)+sizeof(info)+width*height*4),0,0,sizeof(header)+sizeof(info)};
    std::filesystem::create_directories(file.parent_path());
    std::ofstream output(file,std::ios::binary);
    output.write(reinterpret_cast<const char*>(&header),sizeof(header));
    output.write(reinterpret_cast<const char*>(&info),sizeof(info));
    output.write(reinterpret_cast<const char*>(bitmap.pixels),width*height*4);
    check(bool(output),"Snapshot write failed");
}
void run(const std::filesystem::path& file) {
    WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr);
    wc.lpszClassName=L"ClipRope.ChoiceTest";
    check(RegisterClassW(&wc)!=0,"Fixture registration failed");
    auto host=CreateWindowExW(0,wc.lpszClassName,L"Choice fixture",WS_POPUP|WS_VISIBLE,
        20,20,500,500,nullptr,nullptr,wc.hInstance,nullptr);
    check(host!=nullptr,"Fixture window failed");
    Surface surface; surface.attach(host);
    auto choice=choice_box(surface,host,L"Choice",10);
    check(choice!=nullptr,"Choice creation failed");
    SendMessageW(choice,CB_ADDSTRING,0,LPARAM(L"First option"));
    SendMessageW(choice,CB_ADDSTRING,0,LPARAM(L"Second option"));
    SendMessageW(choice,CB_SETCURSEL,0,0);
    surface.card_style(choice,{CardPart::settings_choice,0,L"First option"});
    place(choice,20,20,180,choice_height(choice)); drain();
    COMBOBOXINFO info{sizeof(info)};
    check(GetComboBoxInfo(choice,&info),"Choice information failed");
    Observation observed{surface.colors.panel};
    SetWindowSubclass(info.hwndList,observe,2,DWORD_PTR(&observed));
    BOOL animation{}; SystemParametersInfoW(SPI_GETCOMBOBOXANIMATION,0,&animation,0);
    const auto point=MAKELPARAM((info.rcButton.left+info.rcButton.right)/2,(info.rcButton.top+info.rcButton.bottom)/2);
    auto open=[&] {
        const auto start=std::chrono::steady_clock::now();
        SendMessageW(choice,WM_LBUTTONDOWN,MK_LBUTTON,point);
        SendMessageW(choice,WM_LBUTTONUP,0,point);
        check(SendMessageW(choice,CB_GETDROPPEDSTATE,0,0) && IsWindowVisible(info.hwndList),"Click did not expand the choice");
        const auto revealed=std::chrono::steady_clock::now();
        drain(); UpdateWindow(info.hwndList);
        const auto done=std::chrono::steady_clock::now();
        check(IsWindowVisible(frame_for(choice)),"Completed expansion lacks its shared frame");
        check(!observed.nested,"Frame fitting recursively rewrote the native reveal region");
        return std::pair{std::chrono::duration<double,std::milli>(done-start).count(),
                         std::chrono::duration<double,std::milli>(done-revealed).count()};
    };
    auto close=[&] {
        SendMessageW(choice,CB_SHOWDROPDOWN,FALSE,0); drain();
        check(!IsWindowVisible(info.hwndList) && !IsWindowVisible(frame_for(choice)),"Collapse left a list or frame visible");
    };
    const auto first=open();
    if (animation) check(observed.snapshots>0,"Native animation did not supply a snapshot");
    snapshot(info.hwndList,file); // Also covers animation disabled in Windows.
    check(observed.valid_margin,"Animated snapshot omitted the themed nonclient padding");
    close();
    for (int i=0;i<3;++i) { open(); close(); }
    const auto gdi=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS),user=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    const auto targets=Surface::target_creations(),frames=CardMenuFrame::generations();
    std::vector<double> times,completions;
    for (int i=0;i<20;++i) { auto t=open(); times.push_back(t.first); completions.push_back(t.second); close(); }
    check(observed.valid_margin,"Repeated native animation lost its themed padding");
    const auto gdi_end=GetGuiResources(GetCurrentProcess(),GR_GDIOBJECTS),user_end=GetGuiResources(GetCurrentProcess(),GR_USEROBJECTS);
    check(gdi_end<=gdi && user_end<=user,"Repeated expansion retained window resources");
    check(Surface::target_creations()==targets && CardMenuFrame::generations()==frames,"Warm expansion recreated shared resources");
    std::sort(times.begin(),times.end()); std::sort(completions.begin(),completions.end());
    std::cout << "Animation=" << animation << "; first=" << first.first << " ms; warm p95=" << times[18]
              << " ms; post-animation frame p95=" << completions[18] << " ms; snapshots=" << observed.snapshots
              << "; nested region writes=" << observed.nested << "; GDI " << gdi << " -> " << gdi_end
              << "; USER " << user << " -> " << user_end << "; target/frame creations=0/0\n";
    const auto frame=frame_for(choice);
    // Queued work must not resurrect its owner after destruction.
    SendMessageW(choice,CB_SHOWDROPDOWN,TRUE,0); DestroyWindow(host); drain();
    check(!IsWindow(frame) && !IsWindow(info.hwndList),"Choice destruction retained a popup");
}
}
int wmain(int argc,wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    auto desktop=CreateDesktopW((L"ClipRope.Choices."+std::to_wstring(GetCurrentProcessId())).c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return 2;
    try { run(argc>1 ? std::filesystem::path(argv[1]) : std::filesystem::path{}); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    return 0;
}
