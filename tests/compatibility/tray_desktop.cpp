#include "../integration/history_fixture.hpp"
#include "../integration/clipboard_process.hpp"
#include "platform_win/tray.hpp"
#include "platform_win/tray_popup.hpp"
#include "platform_win/fluent.hpp"
#include <commctrl.h>
#include <oleacc.h>
#include <UIAutomation.h>
#include <wrl.h>
#include <atomic>
#include <shellapi.h>
#include <windowsx.h>
#include <fstream>
#include <iostream>
#include <algorithm>

namespace {
using history_fixture::require;
namespace command = cliprope::win::tray_command;
namespace fs=std::filesystem;
struct Process {
    HANDLE handle{};
    DWORD pid{};
    HWND listener{};
    ~Process() {
        if (!handle) return;
        if (WaitForSingleObject(handle,0)==WAIT_TIMEOUT) {
            if (listener) PostMessageW(listener,WM_CLOSE,0,0);
            if (WaitForSingleObject(handle,3000)!=WAIT_OBJECT_0) {
                TerminateProcess(handle,1); WaitForSingleObject(handle,3000);
            }
        }
        CloseHandle(handle);
    }
};
template<class P> void wait(P predicate,const char* failure) {
    auto end=GetTickCount64()+4500;
    do {
        MSG msg{};
        while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (predicate()) return;
        MsgWaitForMultipleObjects(0,nullptr,FALSE,15,QS_ALLINPUT);
    } while (GetTickCount64()<end);
    throw std::runtime_error(failure);
}
DWORD owner(HWND hwnd) { DWORD pid{}; GetWindowThreadProcessId(hwnd,&pid); return pid; }
HWND find(DWORD pid,const wchar_t* cls) {
    HWND hwnd{};
    while ((hwnd=FindWindowExW(nullptr,hwnd,cls,nullptr))) if (owner(hwnd)==pid) return hwnd;
    return nullptr;
}
void start(Process& proc,const fs::path& exe,const fs::path& profile,const fs::path& report) {
    proc.handle=probe::launch({exe.wstring(),L"--profile",profile.wstring(),L"--probe",report.wstring()},probe::desktop_path());
    require(proc.handle!=nullptr,"Could not launch desktop fixture");
    proc.pid=GetProcessId(proc.handle);
    wait([&]{proc.listener=find(proc.pid,L"ClipRope.Listener"); return proc.listener!=nullptr;},"No host listener");
}
void fixture(const fs::path& exe,const wchar_t* mode,const fs::path& profile) {
    HANDLE h=probe::launch({exe.wstring(),mode,profile.wstring()},probe::desktop_path());
    require(h!=nullptr,"Could not launch data fixture");
    DWORD result=1;
    if (WaitForSingleObject(h,10000)==WAIT_OBJECT_0) GetExitCodeProcess(h,&result);
    CloseHandle(h);
    require(result==0,"Disposable profile check failed");
}
void click(POINT pt,bool right) {
    require(SetCursorPos(pt.x,pt.y)!=0,"Could not position pointer");
    INPUT pair[2]{};
    pair[0].type=pair[1].type=INPUT_MOUSE;
    pair[0].mi.dwFlags=right?MOUSEEVENTF_RIGHTDOWN:MOUSEEVENTF_LEFTDOWN;
    pair[1].mi.dwFlags=right?MOUSEEVENTF_RIGHTUP:MOUSEEVENTF_LEFTUP;
    require(SendInput(2,pair,sizeof(INPUT))==2,"Pointer input failed");
}
POINT center(RECT r) { return {(r.left+r.right)/2,(r.top+r.bottom)/2}; }
std::wstring accessible_name(POINT pt) {
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    Microsoft::WRL::ComPtr<IUIAutomationElement> element;
    if (SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation))) &&
        SUCCEEDED(automation->ElementFromPoint(pt,&element)) && element) {
        BSTR value{};
        element->get_CurrentName(&value);
        std::wstring result=value ? value : L"";
        SysFreeString(value);
        if (!result.empty()) return result;
    }
    IAccessible* node{}; VARIANT child{};
    if (FAILED(AccessibleObjectFromPoint(pt,&node,&child))) return {};
    BSTR name{};
    node->get_accName(child,&name);
    const std::wstring value=name ? name : L"";
    SysFreeString(name); VariantClear(&child); node->Release();
    return value;
}
bool overflow_name(const std::wstring& name) {
    return name==L"Show hidden icons" || name==L"Show hidden icons." ||
           name==L"\u663e\u793a\u9690\u85cf\u7684\u56fe\u6807" || name==L"\u663e\u793a\u9690\u85cf\u56fe\u6807";
}
bool named_icon(std::wstring_view name) {
    return name.find(L"ClipRope")!=name.npos;
}
bool overflow_icon(POINT& pt) {
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    if (FAILED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation)))) return false;
    VARIANT value{}; value.vt=VT_BSTR; value.bstrVal=SysAllocString(L"ClipRope");
    Microsoft::WRL::ComPtr<IUIAutomationCondition> match;
    auto rc=automation->CreatePropertyCondition(UIA_NamePropertyId,value,&match);
    VariantClear(&value);
    if (FAILED(rc)) return false;
    for (const auto cls : {L"TopLevelWindowForOverflowXamlIsland",L"NotifyIconOverflowWindow"}) {
        const auto window=FindWindowW(cls,nullptr);
        if (!window || !IsWindowVisible(window) || owner(window)!=owner(GetShellWindow())) continue;
        Microsoft::WRL::ComPtr<IUIAutomationElement> root,icon;
        RECT rect{};
        if (SUCCEEDED(automation->ElementFromHandle(window,&root)) && root &&
            SUCCEEDED(root->FindFirst(TreeScope_Descendants,match.Get(),&icon)) && icon &&
            SUCCEEDED(icon->get_CurrentBoundingRectangle(&rect)) && !IsRectEmpty(&rect)) {
            const auto p=center(rect);
            if (GetAncestor(WindowFromPoint(p),GA_ROOT)==window) {pt=p;return true;}
        }
    }
    return false;
}
void focus_notification_area(DWORD host_pid) {
    const auto foreground=owner(GetForegroundWindow());
    require(foreground==GetCurrentProcessId() || foreground==host_pid || foreground==owner(GetShellWindow()),
            "Unexpected foreground before revealing the notification area");
    INPUT keys[4]{};
    const WORD vk[]{VK_LWIN,'B','B',VK_LWIN};
    for (int i=0;i<4;++i) { keys[i].type=INPUT_KEYBOARD; keys[i].ki.wVk=vk[i]; keys[i].ki.dwFlags=i>=2?KEYEVENTF_KEYUP:0; }
    require(SendInput(4,keys,sizeof(INPUT))==4,"Could not focus notification area");
}
RECT icon(Process& proc,bool right,bool shell_input) {
    NOTIFYICONIDENTIFIER id{sizeof(id)};
    id.hWnd=proc.listener;
    id.uID=cliprope::win::Tray::icon_id;
    RECT rect{};
    if (!shell_input) {
        POINT pt{350,250};
        const bool located = Shell_NotifyIconGetRect(&id,&rect)==S_OK && !IsRectEmpty(&rect);
        if (!located) rect={pt.x,pt.y,pt.x+1,pt.y+1};
        AllowSetForegroundWindow(proc.pid);
        const LPARAM at = right && located ? MAKELPARAM(-1,-1) : MAKELPARAM(pt.x,pt.y);
        require(PostMessageW(proc.listener,cliprope::win::Tray::callback,at,
                MAKELPARAM(right?WM_CONTEXTMENU:NIN_SELECT,cliprope::win::Tray::icon_id))!=0,"Shell callback dispatch failed");
        return rect;
    }
    HRESULT location=Shell_NotifyIconGetRect(&id,&rect);
    require(SUCCEEDED(location),"Explorer has no icon rectangle");
    const auto display=MonitorFromRect(&rect,MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{sizeof(info)};
    GetMonitorInfoW(display,&info);
    if (!PtInRect(&info.rcMonitor,center(rect))) {
        // Explorer can report its autohidden strip below the actual display.
        const auto p=center(rect);
        SetCursorPos(std::clamp(p.x,info.rcMonitor.left,info.rcMonitor.right-1),
                     std::clamp(p.y,info.rcMonitor.top,info.rcMonitor.bottom-1));
        Sleep(400);
        location=Shell_NotifyIconGetRect(&id,&rect);
        if (!PtInRect(&info.rcMonitor,center(rect))) focus_notification_area(proc.pid);
        wait([&]{ location=Shell_NotifyIconGetRect(&id,&rect);
            return SUCCEEDED(location) && PtInRect(&info.rcMonitor,center(rect));
        },"Explorer did not reveal the autohidden notification area");
        Sleep(300); location=Shell_NotifyIconGetRect(&id,&rect);
    }
    auto pt=center(rect);
    auto name=accessible_name(pt);
    if (overflow_name(name) && owner(WindowFromPoint(pt))==owner(GetShellWindow())) {
        click(pt,false);
        Sleep(350); // Wait for Explorer's overflow grid to finish animating.
        wait([&]{
            if (overflow_icon(pt)) { name=L"ClipRope"; location=S_OK; return true; }
            location=Shell_NotifyIconGetRect(&id,&rect);
            pt=center(rect); name=accessible_name(pt);
            return SUCCEEDED(location) && named_icon(name) && owner(WindowFromPoint(pt))==owner(GetShellWindow());
        },"ClipRope was not exposed inside the system tray overflow");
    }
    // Explorer can report its overflow container at a point even though the
    // child icon is exposed separately in the accessibility tree.
    if (!named_icon(name) && overflow_icon(pt)) { name=L"ClipRope"; location=S_OK; }
    const bool shell=owner(WindowFromPoint(pt))==owner(GetShellWindow());
    if (location!=S_OK || (!name.empty() && !named_icon(name)) || !shell) {
        const auto hit=WindowFromPoint(pt);
        wchar_t cls[128]{},root_cls[128]{};
        GetClassNameW(hit,cls,128); GetClassNameW(GetAncestor(hit,GA_ROOT),root_cls,128);
        std::wcerr << L"icon rect=("<<rect.left<<L","<<rect.top<<L","<<rect.right<<L","<<rect.bottom
                   <<L") hit class="<<cls<<L" root="<<root_cls<<L" has-name="<<!name.empty()
                   <<L" hr="<<std::hex<<location<<std::dec<<L" is-overflow="
                   <<(name.find(L"hidden")!=name.npos || name.find(L"\u9690\u85cf")!=name.npos)<<L"\n";
    }
    require(location==S_OK && (name.empty() || named_icon(name)) && shell,
            "Test icon is not exposed at its Explorer rectangle; pin or reveal ClipRope to run physical icon checks");
    click(pt,right);
    return PtInRect(&rect,pt) ? rect : RECT{pt.x,pt.y,pt.x+1,pt.y+1};
}
HWND show_menu(Process& proc,bool shell_input) {
    const RECT origin = icon(proc,true,shell_input);
    HWND window{};
    wait([&]{window=find(proc.pid,L"ClipRope.TrayMenu"); return window && IsWindowVisible(window);},"No tray popup");
    MONITORINFO display{sizeof(display)};
    RECT face{};
    require(GetMonitorInfoW(MonitorFromRect(&origin,MONITOR_DEFAULTTONEAREST),&display) && GetWindowRect(window,&face),
            "Missing tray placement geometry");
    const RECT work = display.rcWork;
    const LONG gap = MulDiv(2,GetDpiForWindow(window),96);
    const LONG end = std::min(origin.top,work.bottom) - gap;
    require(face.left >= work.left && face.right <= work.right && face.top >= work.top && face.bottom <= work.bottom,
            "Tray menu crosses the work area");
    if (end - (face.bottom-face.top) >= work.top) {
        const LONG start = std::clamp(origin.left,work.left,work.right-(face.right-face.left));
        if (face.left != start || face.bottom != end) {
            std::cerr << "Icon " << origin.left << ',' << origin.top << " face " << face.left << ',' << face.bottom
                      << " expected " << start << ',' << end << '\n';
            throw std::runtime_error("Tray lower-left corner missed its notification icon");
        }
        static bool reported{};
        if (!reported) {
            std::cout << "Tray anchor (" << origin.left << ',' << origin.top << ") "
                      << origin.right-origin.left << 'x' << origin.bottom-origin.top << ", face lower-left ("
                      << face.left << ',' << face.bottom << "), gap " << gap << " px\n";
            reported = true;
        }
    }
    return window;
}
std::wstring caption(HWND popup, int id) {
    wchar_t value[256]{};
    auto control = GetDlgItem(popup, id);
    if (control) SendMessageW(control, WM_GETTEXT, std::size(value), LPARAM(value));
    return value;
}
void choose(HWND popup, int id) {
    const auto control = GetDlgItem(popup, id);
    require(control && IsWindowEnabled(control), "Missing tray button");
    POINT point{};
    // A visible management window may still be completing its measured layout.
    try {
        wait([&]{
            RECT bounds{};
            if (!GetWindowRect(control, &bounds)) return false;
            point = center(bounds);
            return WindowFromPoint(point) == control;
        }, "Expected tray button is covered");
    } catch (...) {
        wchar_t target[96]{}, hit[96]{}, parent[96]{};
        GetClassNameW(control, target, 96);
        GetClassNameW(WindowFromPoint(point), hit, 96);
        GetClassNameW(popup, parent, 96);
        std::wcerr << L"Control " << id << L" (" << target << L") in " << parent << L" at "
                   << point.x << L',' << point.y << L" hit " << hit << L'\n';
        throw;
    }
    click(point, false);
    wait([&]{ return !IsWindow(popup); }, "Action did not close tray popup");
}
void held_press(HWND button, bool cancel) {
    RECT r{};
    require(button && GetWindowRect(button, &r), "Missing press target");
    const POINT at = center(r);
    wait([&]{ return WindowFromPoint(at) == button; }, "Press target is covered");
    require(SetCursorPos(at.x, at.y) != FALSE, "Could not reach press target");
    INPUT mouse{};
    mouse.type = INPUT_MOUSE;
    mouse.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    require(SendInput(1, &mouse, sizeof(mouse)) == 1, "Could not press button");
    struct Release {
        INPUT mouse;
        ~Release() { mouse.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1, &mouse, sizeof(mouse)); }
    } release{mouse};
    auto pushed = [&]{ return (SendMessageW(button, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0; };
    wait(pushed, "Button did not enter the pressed state");
    Sleep(200);
    require(pushed(), "Button did not sustain a held press");
    if (cancel) {
        require(SetCursorPos(r.left - 2, at.y) != FALSE, "Could not drag off button");
        wait([&]{ return !pushed(); }, "Dragging outside did not disarm button");
    }
}
void automation_invoke(HWND window, int id) {
    const auto button = GetDlgItem(window, id);
    require(button != nullptr, "Automation target is missing");
    Microsoft::WRL::ComPtr<IUIAutomation> client;
    Microsoft::WRL::ComPtr<IUIAutomationElement> element;
    Microsoft::WRL::ComPtr<IUIAutomationInvokePattern> invoke;
    require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&client))) &&
            SUCCEEDED(client->ElementFromHandle(button, &element)) && element &&
            SUCCEEDED(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) && invoke &&
            SUCCEEDED(invoke->Invoke()), "Could not invoke native button through UI Automation");
}
void key(HWND popup, WORD vk) {
    wait([&]{ return GetForegroundWindow() == popup; }, "Tray popup did not take keyboard focus");
    INPUT keys[2]{};
    keys[0].type = keys[1].type = INPUT_KEYBOARD;
    keys[0].ki.wVk = keys[1].ki.wVk = vk;
    keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
    require(SendInput(2, keys, sizeof(INPUT)) == 2, "Tray key dispatch failed");
}
void shortcut(HWND expected) {
    require(GetForegroundWindow()==expected,"Selected fixture field lost foreground");
    INPUT keys[6]{};
    const WORD key[]{VK_CONTROL,VK_MENU,VK_F11,VK_F11,VK_MENU,VK_CONTROL};
    for (int i=0;i<6;++i) { keys[i].type=INPUT_KEYBOARD; keys[i].ki.wVk=key[i]; keys[i].ki.dwFlags=i>=3?KEYEVENTF_KEYUP:0; }
    require(SendInput(6,keys,sizeof(INPUT))==6,"Could not type rebound shortcut");
}
void select(HWND form,HWND field) {
    SetForegroundWindow(form);
    SetFocus(field);
    if (GetForegroundWindow()!=form || GetFocus()!=field) {
        SetWindowPos(form,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        RECT rect{}; GetWindowRect(field,&rect);
        const auto p=center(rect);
        require(WindowFromPoint(p)==field,"Controlled field is covered by another window");
        click(p,false);
        wait([&]{return GetForegroundWindow()==form && GetFocus()==field;},"Field click did not take focus");
        SetWindowPos(form,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    }
    require(GetForegroundWindow()==form && GetFocus()==field,"External field could not take focus");
}
void verify_field(HWND field) {
    wchar_t buf[64]{};
    GetWindowTextW(field,buf,64);
    require(std::wstring_view(buf)==L"unchanged","Tray recall inserted into the external field");
}
void verify_published(DWORD pid) {
    wait([&]{
        if (owner(GetClipboardOwner())!=pid || !OpenClipboard(nullptr)) return false;
        bool match=false;
        if (owner(GetClipboardOwner())==pid) {
            auto data=GetClipboardData(CF_UNICODETEXT);
            if (data && GlobalSize(data)>=sizeof(L"Tray sample 5")) {
                auto p=GlobalLock(data);
                if (p) { match=std::memcmp(p,L"Tray sample 5",sizeof(L"Tray sample 5"))==0; GlobalUnlock(data); }
            }
        }
        CloseClipboard();
        return match;
    },"Expected synthetic clipboard value was not published");
}
void log_contains(const fs::path& file,std::string_view fragment, std::streamoff from = 0) {
    wait([&]{
        std::ifstream input(file);
        input.seekg(from);
        const std::string text(std::istreambuf_iterator<char>(input),{});
        return text.find(fragment)!=text.npos;
    },"Expected host outcome was not observed");
}
void button_interaction(const fs::path& host, const fs::path& helper) {
    require(!FindWindowW(L"ClipRope.Listener", nullptr), "Close other ClipRope instances before desktop input checks");
    history_fixture::Directory temp;
    const auto profile = temp.path / "buttons", report = temp.path / "report";
    fixture(helper, L"--empty", profile);
    Process app;
    start(app, host, profile, report);
    log_contains(report, "ready");
    auto form = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"EDIT", L"Button input fixture",
                                WS_POPUP | WS_VISIBLE | ES_READONLY, 40, 100, 120, 50, nullptr, nullptr, nullptr, nullptr);
    require(form != nullptr, "Cannot create input fixture");
    struct Cleanup {
        HWND form;
        POINT cursor{};
        bool restore = GetCursorPos(&cursor) != FALSE;
        ~Cleanup() { DestroyWindow(form); if (restore) SetCursorPos(cursor.x, cursor.y); }
    } cleanup{form};
    auto menu = [&] {
        RECT r{};
        GetWindowRect(form, &r);
        const auto at = center(r);
        wait([&]{return WindowFromPoint(at) == form;}, "Synthetic input fixture is covered");
        click(at, false);
        wait([&]{return GetForegroundWindow() == form;}, "Cannot activate input fixture");
        require(AllowSetForegroundWindow(app.pid) != FALSE, "Cannot grant test host foreground input");
        PostMessageW(app.listener, cliprope::win::Tray::callback, MAKELPARAM(at.x, at.y),
                     MAKELPARAM(WM_CONTEXTMENU, cliprope::win::Tray::icon_id));
        HWND popup{};
        wait([&]{popup = find(app.pid, L"ClipRope.TrayMenu"); return popup && GetForegroundWindow() == popup;},
             "Tray callback did not present an active menu");
        return popup;
    };
    auto settings = [&] {
        HWND dialog{};
        wait([&]{dialog = find(app.pid, L"ClipRope.Settings"); return dialog && IsWindowVisible(dialog);},
             "Tray action did not open Settings");
        return dialog;
    };
    const auto close_id = cliprope::win::fluent::dialog_close_id;
    HWND popup = menu();
    const auto header = GetDlgItem(popup, command::open);
    held_press(header, true);
    GUITHREADINFO info{sizeof(info)};
    const DWORD thread = GetWindowThreadProcessId(header, nullptr);
    wait([&]{return GetGUIThreadInfo(thread, &info) && !info.hwndCapture;}, "Header retained capture");
    auto workspace = find(app.pid, L"ClipRope.Workspace");
    require(IsWindow(header) && workspace && !IsWindowVisible(workspace), "Cancelled header activated workspace");
    held_press(header, false);
    wait([&]{return !IsWindow(popup) && IsWindowVisible(workspace);}, "Held header did not open workspace");
    SendMessageW(workspace, WM_CLOSE, 0, 0);
    wait([&]{return !IsWindowVisible(workspace);}, "Workspace did not collapse");

    popup = menu();
    automation_invoke(popup, command::settings);
    auto dialog = settings();
    auto close = GetDlgItem(dialog, close_id);
    held_press(close, true);
    wait([&]{return GetGUIThreadInfo(thread, &info) && !info.hwndCapture && info.hwndFocus == close;},
         "Cancelled close lost the window or focus");
    key(dialog, VK_SPACE);
    wait([&]{return !IsWindow(dialog);}, "Space did not close Settings");

    choose(menu(), command::settings);
    choose(settings(), close_id);
    key(menu(), 'S');
    dialog = settings();
    automation_invoke(dialog, close_id);
    wait([&]{return !IsWindow(dialog);}, "UI Automation did not close Settings");
}
void limited_hover(HWND listener) {
    using namespace cliprope;
    win::TrayActions actions;
    actions.read_shortcuts = [] {
        auto keys = application::default_shortcuts();
        keys[application::workspace_toggle].enabled = keys[application::quick_note].enabled = false;
        return keys;
    };
    actions.hover_status = [] { return std::optional<DWORD>(ERROR_ACCESS_DENIED); };
    win::Tray tray(std::move(actions));
    require(tray.start(listener), "Could not start limited-hover fixture");
    tray.message(WM_COMMAND, command::settings, 0);
    auto settings = find(GetCurrentProcessId(), L"ClipRope.Settings");
    SendMessageW(GetDlgItem(settings, 1503), BM_CLICK, 0, 0);
    auto page = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
    auto check = GetDlgItem(page, 1810);
    require(check != nullptr, "Scoped check button missing");
    SetFocus(check);
    SendMessageW(check, BM_CLICK, 0, 0);
    wchar_t label[128]{};
    GetWindowTextW(check, label, 128);
    require(std::wstring_view(label).starts_with(L"Limited"), "Hook failure reported full availability");
    HWND tip{};
    while ((tip = FindWindowExW(nullptr, tip, L"ClipRope.Tooltip", nullptr)) && GetWindow(tip, GW_OWNER) != settings) {}
    wchar_t reason[256]{};
    SendMessageW(tip, WM_GETTEXT, 256, LPARAM(reason));
    require(tip != nullptr, "Limited result did not create a tip");
    require(IsWindowVisible(tip), "Limited result tip is hidden");
    require(std::wstring_view(reason).find(L"Workspace focus works") != std::wstring_view::npos,
            "Limited result lost its functioning input scope");
    const HWND focus = GetFocus();
    MSG esc{check, WM_KEYDOWN, VK_ESCAPE};
    require(tray.dialog_message(esc) && !IsWindowVisible(tip) && IsWindow(settings) && GetFocus() == focus,
            "Escape did not dismiss just the feedback tip");
    SendMessageW(settings, WM_CLOSE, 0, 0);
}
void run(const fs::path& host,const fs::path& helper,bool shell_input) {
    require(!FindWindowW(L"ClipRope.Listener",nullptr),"Close other ClipRope instances before shell qualification");
    history_fixture::Directory temp;
    struct Cursor { POINT p{}; bool ok=GetCursorPos(&p)!=FALSE; ~Cursor(){if(ok)SetCursorPos(p.x,p.y);} } cursor;
    HWND form=CreateWindowExW(WS_EX_TOOLWINDOW,L"STATIC",L"ClipRope shortcut probe",WS_OVERLAPPEDWINDOW,
                              30,400,250,110,nullptr,nullptr,nullptr,nullptr);
    HWND field=CreateWindowW(L"EDIT",L"unchanged",WS_CHILD|WS_VISIBLE|WS_BORDER,
                             10,15,205,26,form,nullptr,nullptr,nullptr);
    require(form && field,"Cannot create controlled external field");
    ShowWindow(form,SW_SHOW);
    limited_hover(form);
    const auto profile=temp.path/"seed",log=temp.path/"report.txt";
    fixture(helper,L"--seed",profile);
    const bool reserved=RegisterHotKey(form,90,MOD_CONTROL|MOD_ALT,VK_F11)!=FALSE;
    Process app;
    start(app,host,profile,log);
    HWND settings{};
    wait([&]{settings=find(app.pid,L"ClipRope.Settings");return settings && IsWindowVisible(settings);},"No visible shortcut conflict");
    auto page = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
    auto status=GetDlgItem(page, 1802);
    wchar_t explanation[256]{};
    SendMessageW(status,WM_GETTEXT,256,reinterpret_cast<LPARAM>(explanation));
    require(std::wstring_view(explanation).find(L"Unavailable")!=std::wstring_view::npos,"Conflict explanation is missing");
    log_contains(log,"hotkey conflict");
    require(IsWindowEnabled(GetDlgItem(page, 1801)), "Conflicting binding is not editable");
    if (reserved) {
        UnregisterHotKey(form,90);
        SendMessageW(GetDlgItem(page, 1802), BM_CLICK, 0, 0);
        require(caption(page, 1802).find(L"✓ Registered") != std::wstring::npos, "Availability check did not recover the global binding");
    }
    SendMessageW(settings,WM_CLOSE,0,0);
    auto workspace=find(app.pid,L"ClipRope.Workspace");
    require(workspace && !IsWindowVisible(workspace),"Workspace should start hidden");
    select(form,field);
    icon(app,false,shell_input);
    wait([&]{return IsWindowVisible(workspace);},"Tray activation did not show workspace");
    icon(app,false,shell_input);
    wait([&]{return !IsWindowVisible(workspace);},"Tray activation did not hide workspace");
    select(form,field);
    auto popup=show_menu(app,shell_input);
    const auto sequence = GetClipboardSequenceNumber();
    auto source = CreateWindowW(L"EDIT",L"",WS_CHILD,0,0,80,20,form,nullptr,nullptr,nullptr);
    auto copy = [&](const wchar_t* text) {
        const auto offset = fs::file_size(log);
        SetWindowTextW(source,text);
        SendMessageW(source,EM_SETSEL,0,-1);
        SendMessageW(source,WM_COPY,0,0);
        log_contains(log,"capture captured ",static_cast<std::streamoff>(offset));
    };
    auto check_rows = [&](HWND menu) {
        require(caption(menu,command::open).find(L"Current rope: main") != std::wstring::npos &&
                caption(menu,command::settings) == L"Global settings" && caption(menu,command::exit) == L"Exit" &&
                !GetDlgItem(menu,command::head) && !GetDlgItem(menu,command::first_preview + 5),
                "Tray sections or summary limit mismatch");
        for (int i = 0; i < 5; ++i)
            require(caption(menu,command::first_preview+i).starts_with(std::to_wstring(i+1) + L". Tray sample " +
                    std::to_wstring(6-i)), "Frozen menu summary mismatch");
    };
    check_rows(popup);
    key(popup,VK_ESCAPE);
    wait([&]{return !IsWindow(popup);}, "Escape did not dismiss tray");
    if (shell_input)
        wait([&]{return owner(GetForegroundWindow()) == owner(GetShellWindow());}, "Escape did not return focus to Explorer");
    popup = show_menu(app,shell_input);
    choose(popup,command::open);
    wait([&]{return IsWindowVisible(workspace);}, "Tray header did not open the workspace");
    popup = show_menu(app,shell_input);
    key(popup,VK_TAB);
    key(popup,VK_DOWN);
    wait([&]{
        GUITHREADINFO info{sizeof(info)};
        return GetGUIThreadInfo(GetWindowThreadProcessId(popup,nullptr), &info) &&
               info.hwndFocus == GetDlgItem(popup,command::first_preview);
    }, "Arrow navigation missed the first preview");
    key(popup,VK_UP);
    key(popup,VK_RETURN);
    wait([&]{return !IsWindow(popup);}, "Keyboard header activation did not close tray");
    require(IsWindowVisible(workspace), "Header hid the open workspace");
    popup = show_menu(app,shell_input);
    key(popup,'S');
    wait([&]{ settings = find(app.pid,L"ClipRope.Settings"); return settings && IsWindowVisible(settings); },
         "Settings mnemonic did not open the center");
    SendMessageW(settings,WM_CLOSE,0,0);
    popup = show_menu(app,shell_input);
    RECT outside{};
    SetWindowPos(form,HWND_TOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    GetWindowRect(field,&outside);
    require(WindowFromPoint(center(outside)) == field, "External test field is covered");
    click(center(outside),false);
    wait([&]{return !IsWindow(popup);}, "Outside click did not dismiss tray");
    SetWindowPos(form,HWND_NOTOPMOST,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    popup = show_menu(app,shell_input);
    check_rows(popup);
    require(GetClipboardSequenceNumber() == sequence, "Navigation changed the clipboard");
    copy(L"New tray sample A");
    choose(popup,command::first_preview+1);
    log_contains(log,"invoke tray ");
    verify_published(app.pid);
    verify_field(field);
    select(form,field);
    popup=show_menu(app,shell_input);
    require(caption(popup,command::first_preview).find(L"Tray sample 5")!=std::wstring::npos,"Published item not at head");
    key(popup,'1');
    wait([&]{return !IsWindow(popup);}, "Number key did not activate row one");
    verify_published(app.pid);
    popup=show_menu(app,shell_input);
    key(popup,'H');
    log_contains(log,"invoke head ");
    verify_published(app.pid);
    verify_field(field);
    popup=show_menu(app,shell_input);
    copy(L"New tray sample B");
    copy(L"New tray sample C");
    const auto before_stale = GetClipboardSequenceNumber();
    key(popup,'5');
    wait([&]{ return !IsWindow(popup); }, "Stale preview activation did not close the menu");
    popup=show_menu(app,shell_input);
    require(GetClipboardSequenceNumber() == before_stale && caption(popup,command::first_preview).find(L"New tray sample C") !=
            std::wstring::npos, "Stale preview published or promoted a replacement");
    key(popup,'X');
    wait([&]{return WaitForSingleObject(app.handle,0)==WAIT_OBJECT_0;}, "Tray Exit failed");
    fixture(helper,L"--verify",profile);
    const auto empty=temp.path/"empty";
    fixture(helper,L"--empty",empty);
    Process blank;
    start(blank,host,empty,temp.path/"blank.log");
    log_contains(temp.path/"blank.log","ready");
    select(form,field);
    shortcut(form);
    wait([&]{return IsWindowVisible(find(blank.pid,L"ClipRope.Workspace"));},"Persisted shortcut did not expand workspace");
    shortcut(form);
    wait([&]{return !IsWindowVisible(find(blank.pid,L"ClipRope.Workspace"));},"Persisted shortcut did not hide workspace");
    popup=show_menu(blank,shell_input);
    require(caption(popup,command::empty) == L"No recent cards" && !IsWindowEnabled(GetDlgItem(popup,command::empty))
            && !GetDlgItem(popup,command::first_preview), "Empty rope offered a preview");
    const auto empty_sequence = GetClipboardSequenceNumber();
    key(popup,'H');
    key(popup,'1');
    choose(popup,command::exit);
    require(GetClipboardSequenceNumber() == empty_sequence, "Empty recall changed the clipboard");
    wait([&]{return WaitForSingleObject(blank.handle,0)==WAIT_OBJECT_0;}, "Closing empty host failed");
    DestroyWindow(form);
    std::cout << "editable-conflict=1 saved-shortcut=Ctrl+Alt+F11 shell-input=" << shell_input
              << " summaries=5 frozen-target=ok stale-target=ok external-edits=0 empty-head=disabled exit=ok\n";
}
}
namespace {
class FeedbackListener final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IUIAutomationNotificationEventHandler> {
  public:
    std::atomic_bool received{};
    HRESULT STDMETHODCALLTYPE HandleNotificationEvent(IUIAutomationElement*, NotificationKind, NotificationProcessing,
                                                       BSTR message, BSTR) override {
        if (message && std::wstring_view(message) == L"Show or hide ClipRope. Registered") received = true;
        return S_OK;
    }
};
void observe_feedback(HWND button) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IUIAutomation5> client;
    ComPtr<IUIAutomationElement> view;
    auto observer = Microsoft::WRL::Make<FeedbackListener>();
    require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&client))) &&
            SUCCEEDED(client->ElementFromHandle(GetParent(button), &view)), "Viewport automation unavailable");
    require(SUCCEEDED(client->AddNotificationEventHandler(view.Get(), TreeScope_Element, nullptr, observer.Get())),
            "Could not subscribe to shortcut notifications");
    SendMessageW(button, BM_CLICK, 0, 0);
    wait([&] { return observer->received.load(); }, "Shortcut result did not reach the UI Automation client");
    ComPtr<IAccessible> control;
    VARIANT child{};
    child.vt = VT_I4;
    child.lVal = CHILDID_SELF;
    BSTR description{};
    require(SUCCEEDED(AccessibleObjectFromWindow(button, DWORD(OBJID_CLIENT), IID_PPV_ARGS(&control))) &&
            SUCCEEDED(control->get_accDescription(child, &description)), "Missing button description");
    const bool correct = description && std::wstring_view(description) == L"Registered";
    SysFreeString(description);
    require(correct, "Unexpected availability description");
    client->RemoveNotificationEventHandler(view.Get(), observer.Get());
}
} // namespace
int wmain(int argc,wchar_t** argv) {
    if (argc == 3 && std::wstring_view(argv[1]) == L"--shortcut-notification") {
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 1;
        try { observe_feedback(reinterpret_cast<HWND>(std::wcstoull(argv[2], nullptr, 10))); }
        catch (const std::exception& e) { std::cerr << e.what() << '\n'; CoUninitialize(); return 1; }
        CoUninitialize();
        return 0;
    }
    if(argc==3 && std::wstring_view(argv[1])==L"--automation") {
        if(FAILED(CoInitializeEx(nullptr,COINIT_MULTITHREADED))) return 1;
        int result=1;
        {
            Microsoft::WRL::ComPtr<IUIAutomation> client;
            Microsoft::WRL::ComPtr<IUIAutomationElement> element;
            auto h=reinterpret_cast<HWND>(std::wcstoull(argv[2],nullptr,10));
            if(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&client))) &&
               SUCCEEDED(client->ElementFromHandle(h,&element)) && element) {
                CONTROLTYPEID role{};BOOL enabled{};BSTR label{};
                element->get_CurrentControlType(&role);element->get_CurrentIsEnabled(&enabled);element->get_CurrentName(&label);
                std::cout<<role<<"|"<<!!enabled<<"|";
                for(auto pattern:{UIA_ValuePatternId,UIA_TogglePatternId,UIA_InvokePatternId,UIA_SelectionItemPatternId,UIA_RangeValuePatternId,UIA_TextPatternId,UIA_ScrollPatternId}) {
                    Microsoft::WRL::ComPtr<IUnknown> action;
                    if(SUCCEEDED(element->GetCurrentPattern(pattern,&action)) && action) std::cout<<pattern<<",";
                }
                const int chars = SysStringLen(label);
                std::string name(WideCharToMultiByte(CP_UTF8,0,label,chars,nullptr,0,nullptr,nullptr), '\0');
                if (!name.empty()) WideCharToMultiByte(CP_UTF8,0,label,chars,name.data(),int(name.size()),nullptr,nullptr);
                std::cout<<"|"<<name<<"\n";SysFreeString(label);result=0;
            }
        }
        CoUninitialize();return result;
    }
    if (argc!=4) return 2;
    auto mode=std::wstring_view(argv[1]);
    if (mode!=L"--physical" && mode!=L"--shell-callback" && mode!=L"--buttons") return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(OleInitialize(nullptr))) return 1;
    int status=1;
    try {
        if (mode == L"--buttons") button_interaction(fs::absolute(argv[2]), fs::absolute(argv[3]));
        else run(fs::absolute(argv[2]),fs::absolute(argv[3]),mode==L"--physical");
        status=0;
    }
    catch(const std::exception& e) { std::cerr << e.what() << '\n'; }
    OleUninitialize();
    return status;
}
