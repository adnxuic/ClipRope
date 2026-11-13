#include "platform_win/fluent_list.hpp"
#include "platform_win/fluent_scroll.hpp"
#include "platform_win/card_menu_frame.hpp"
#include <algorithm>
#include <commctrl.h>
#include <windowsx.h>

namespace cliprope::win::fluent {
namespace {
constexpr int row_height = 32, padding = 4, gutter = 16;
constexpr UINT fit_choice_frame = WM_APP + 0x466;
struct ListSkin;
LRESULT CALLBACK list_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK owner_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK choice_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
struct ListSkin {
    Surface& surface;
    HWND list, combo{}, owner{};
    ScrollBar scroll;
    CardMenuFrame frame;
    int hot{-1}, maximum{};
    bool syncing{}, frame_pending{};

    void defer_frame() {
        if (combo && !frame_pending)
            frame_pending = PostMessageW(list,fit_choice_frame,0,0) != FALSE;
    }
    void fit_frame() {
        frame_pending = false;
        if (!IsWindowVisible(list)) return;
        if (!frame.window()) frame.open(combo);
        if (frame.window()) {
            frame.fit(list,surface.colors,design_dpi(GetDpiForWindow(list)),8);
            SetWindowPos(frame.window(),list,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        }
    }

    int count() const { return std::max(0, int(SendMessageW(list, LB_GETCOUNT, 0, 0))); }
    void synchronize() {
        if (syncing || !IsWindow(list)) return;
        syncing = true;
        RECT area{}; GetClientRect(list, &area);
        const int page = std::max(1, int(area.bottom) / px(list,row_height));
        maximum = std::max(0,count()-page);
        const auto position = std::clamp(int(SendMessageW(list,LB_GETTOPINDEX,0,0)),0,maximum);
        if (int(SendMessageW(list,LB_GETTOPINDEX,0,0)) != position) SendMessageW(list,LB_SETTOPINDEX,position,0);
        scroll.update(position, maximum, page, 1);
        MoveWindow(scroll.window(), std::max(0L, area.right-px(list,gutter)),0,px(list,gutter),area.bottom,TRUE);
        syncing = false;
    }
    void metrics() {
        SendMessageW(list,LB_SETITEMHEIGHT,0,px(list,row_height));
        synchronize();
        defer_frame();
    }
    void draw(const DRAWITEMSTRUCT& item) {
        if (item.itemID == UINT(-1)) return;
        const auto length = SendMessageW(list,LB_GETTEXTLEN,item.itemID,0);
        if (length < 0) return;
        std::wstring label(size_t(length)+1,L'\0');
        if (SendMessageW(list,LB_GETTEXT,item.itemID,LPARAM(label.data())) < 0) return;
        label.resize(size_t(length));
        RECT bounds = item.rcItem;
        solid(item.hDC,bounds,surface.colors.panel);
        if (maximum) bounds.right -= px(list,gutter);
        // Owner-draw coordinates are logical; WM_PRINT supplies a translated DC.
        // BindDC uses device bounds, so preserve the native client inset explicitly.
        const auto saved = SaveDC(item.hDC);
        LPtoDP(item.hDC,reinterpret_cast<POINT*>(&bounds),2);
        SetMapMode(item.hDC,MM_TEXT);
        SetWindowOrgEx(item.hDC,0,0,nullptr);
        SetViewportOrgEx(item.hDC,0,0,nullptr);
        surface.draw_card(list,item.hDC,bounds,
            {CardPart::list_item,0,UiText(std::move(label)),{},{},0,(item.itemState & ODS_SELECTED) != 0},
            hot == int(item.itemID));
        RestoreDC(item.hDC,saved);
    }
    void border() {
        auto dc = GetWindowDC(list);
        if (!dc) return;
        RECT area{}, client{};
        GetWindowRect(list,&area); GetClientRect(list,&client);
        MapWindowPoints(list,nullptr,reinterpret_cast<POINT*>(&client),2);
        OffsetRect(&client,-area.left,-area.top);
        OffsetRect(&area,-area.left,-area.top);
        ExcludeClipRect(dc,client.left,client.top,client.right,client.bottom);
        surface.draw_card(list,dc,area,{CardPart::panel});
        ReleaseDC(list,dc);
    }
};
bool attach_list(Surface& surface, HWND list, HWND owner, HWND combo = nullptr) {
    auto skin = std::make_unique<ListSkin>(ListSkin{surface,list,combo,owner});
    auto* context = skin.get();
    if (!SetWindowSubclass(list,list_proc,1,DWORD_PTR(context))) return false;
    if (!SetWindowSubclass(owner,owner_proc,UINT_PTR(list),DWORD_PTR(context))) {
        RemoveWindowSubclass(list,list_proc,1); return false;
    }
    skin.release(); // The native list owns its skin through WM_NCDESTROY.
    SetPropW(list,L"ClipRope.FluentList",HANDLE(1));
    SetWindowLongPtrW(list,GWL_STYLE,GetWindowLongPtrW(list,GWL_STYLE)&~(WS_BORDER|WS_VSCROLL|WS_HSCROLL));
    SetWindowLongPtrW(list,GWL_EXSTYLE,GetWindowLongPtrW(list,GWL_EXSTYLE)&~WS_EX_CLIENTEDGE);
    if (!context->scroll.create(list,nullptr,surface.colors,true,[context](int position) {
        SendMessageW(context->list,LB_SETTOPINDEX,position,0);
        context->synchronize();
        InvalidateRect(context->list,nullptr,FALSE);
    })) return false;
    SetWindowPos(list,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_FRAMECHANGED);
    context->metrics();
    return true;
}
LRESULT CALLBACK owner_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto& skin = *reinterpret_cast<ListSkin*>(data);
    if (m == WM_DRAWITEM && l) {
        auto& item = *reinterpret_cast<DRAWITEMSTRUCT*>(l);
        if (item.hwndItem == skin.list || item.hwndItem == skin.combo) {
            if (item.itemState & ODS_COMBOBOXEDIT) return DefSubclassProc(h,m,w,l);
            skin.draw(item); return TRUE;
        }
    }
    if (m == WM_CTLCOLORLISTBOX && HWND(l) == skin.list)
        return skin.surface.control_color(HDC(w),skin.list,true);
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h,owner_proc,id);
    return DefSubclassProc(h,m,w,l);
}
LRESULT CALLBACK list_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    auto& skin = *reinterpret_cast<ListSkin*>(data);
    switch (m) {
    case fit_choice_frame: skin.fit_frame(); return 0;
    case WM_PRINT: {
        if ((l & PRF_CHECKVISIBLE) && !IsWindowVisible(h)) return 0;
        // AnimateWindow snapshots WM_PRINT, not our screen-only WM_NCPAINT.
        // Paint its nonclient padding into the supplied DC before the native items.
        const auto dc = HDC(w);
        const auto saved = SaveDC(dc);
        if (l & PRF_NONCLIENT) {
            RECT bounds{}; GetWindowRect(h,&bounds);
            OffsetRect(&bounds,-bounds.left,-bounds.top);
            skin.surface.draw_card(h,dc,bounds,{CardPart::panel});
        }
        RECT window{}, client{};
        GetWindowRect(h,&window); GetClientRect(h,&client);
        POINT origin{}; ClientToScreen(h,&origin);
        OffsetViewportOrgEx(dc,origin.x-window.left,origin.y-window.top,nullptr);
        IntersectClipRect(dc,client.left,client.top,client.right,client.bottom);
        const auto result = DefWindowProcW(h,m,w,l & ~PRF_NONCLIENT);
        RestoreDC(dc,saved);
        return result;
    }
    case WM_WINDOWPOSCHANGING:
        if (skin.combo && !(reinterpret_cast<WINDOWPOS*>(l)->flags & SWP_NOSIZE)) {
            auto& position = *reinterpret_cast<WINDOWPOS*>(l);
            MONITORINFO monitor{sizeof(monitor)};
            GetMonitorInfoW(MonitorFromWindow(skin.combo,MONITOR_DEFAULTTONEAREST),&monitor);
            RECT anchor{}; GetWindowRect(skin.combo,&anchor);
            const auto work = monitor.rcWork;
            const int wanted = std::max(1,skin.count())*px(h,row_height)+2*px(h,padding);
            const int below = std::max(0L,work.bottom-anchor.bottom), above = std::max(0L,anchor.top-work.top);
            const bool upwards = below < wanted && above > below;
            position.cy = std::min(wanted,upwards ? above : below);
            position.y = upwards ? anchor.top-position.cy : anchor.bottom;
            position.cx = std::min<int>(position.cx,work.right-work.left);
            position.x = std::clamp<int>(position.x,work.left,work.right-position.cx);
        }
        break;
    case WM_NCCALCSIZE: {
        auto& r = w ? reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0] : *reinterpret_cast<RECT*>(l);
        InflateRect(&r,-px(h,padding),-px(h,padding));
        return 0;
    }
    case WM_NCPAINT: skin.border(); return 0;
    case WM_MOUSEMOVE: {
        const auto hit = SendMessageW(h,LB_ITEMFROMPOINT,0,l);
        const int item = HIWORD(hit) ? -1 : int(LOWORD(hit));
        if (skin.hot != item) {
            skin.hot = item;
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0};
            TrackMouseEvent(&tracking);
            InvalidateRect(h,nullptr,FALSE);
        }
        break;
    }
    case WM_MOUSELEAVE: skin.hot = -1; InvalidateRect(h,nullptr,FALSE); break;
    case WM_MOUSEWHEEL: skin.scroll.wheel(w); return 0;
    case WM_SHOWWINDOW:
        if (!w && skin.frame.window()) ShowWindow(skin.frame.window(),SW_HIDE);
        if (w) skin.defer_frame();
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(skin.owner,owner_proc,UINT_PTR(h));
        RemoveWindowSubclass(h,list_proc,1);
        RemovePropW(h,L"ClipRope.FluentList");
        delete &skin;
        return DefSubclassProc(h,m,w,l);
    }
    const auto result = DefSubclassProc(h,m,w,l);
    DWORD_PTR alive{};
    if (!GetWindowSubclass(h,list_proc,1,&alive) || alive != data) return result;
    switch (m) {
    case WM_WINDOWPOSCHANGED: {
        const auto flags = reinterpret_cast<WINDOWPOS*>(l)->flags;
        if ((flags & SWP_HIDEWINDOW) && skin.frame.window()) ShowWindow(skin.frame.window(),SW_HIDE);
        // Native expansion adjusts the region each frame. Refitting here would
        // replace that reveal region and expose/stretch the unfinished snapshot.
        if ((flags & SWP_SHOWWINDOW) || !(flags & SWP_NOSIZE) || !(flags & SWP_NOMOVE))
            skin.defer_frame();
        break;
    }
    case WM_SIZE: skin.defer_frame(); skin.synchronize(); break;
    case WM_KEYDOWN: case WM_LBUTTONUP:
    case LB_ADDSTRING: case LB_INSERTSTRING: case LB_DELETESTRING: case LB_RESETCONTENT:
    case LB_SETCURSEL: case LB_SETTOPINDEX: case WM_VSCROLL:
        skin.synchronize(); break;
    case WM_SETFONT: case WM_DPICHANGED_AFTERPARENT: skin.metrics(); break;
    case WM_SETFOCUS: case WM_KILLFOCUS: case WM_ENABLE: case WM_UPDATEUISTATE:
        RedrawWindow(h,nullptr,nullptr,RDW_INVALIDATE|RDW_FRAME); break;
    }
    return result;
}
LRESULT CALLBACK choice_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    const auto result = DefSubclassProc(h,m,w,l);
    if (!IsWindow(h)) return result;
    if (m == WM_SETFONT || m == WM_DPICHANGED_AFTERPARENT) {
        SendMessageW(h,CB_SETITEMHEIGHT,WPARAM(-1),px(h,30));
        SendMessageW(h,CB_SETITEMHEIGHT,0,px(h,row_height));
    }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h,choice_proc,1);
    return result;
}
}
HWND list_box(Surface& surface, HWND parent, UiText name, int id) {
    auto h = child(parent,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|LBS_NOINTEGRALHEIGHT|
                   LBS_OWNERDRAWFIXED|LBS_HASSTRINGS,id);
    if (!h) return nullptr;
    surface.font(h);
    accessible_name(h,std::move(name));
    if (!attach_list(surface,h,parent)) { DestroyWindow(h); return nullptr; }
    return h;
}
HWND choice_box(Surface& surface, HWND parent, UiText name, int id) {
    auto h = child(parent,L"COMBOBOX",L"",WS_TABSTOP|CBS_DROPDOWNLIST|CBS_NOINTEGRALHEIGHT|
                   CBS_OWNERDRAWFIXED|CBS_HASSTRINGS,id);
    if (!h) return nullptr;
    COMBOBOXINFO info{sizeof(info)};
    if (!GetComboBoxInfo(h,&info) || !attach_list(surface,info.hwndList,parent,h) ||
        !SetWindowSubclass(h,choice_proc,1,0)) { DestroyWindow(h); return nullptr; }
    surface.font(h);
    accessible_name(h,std::move(name));
    SetPropW(h,L"ClipRope.FluentChoice",HANDLE(1));
    return h;
}
int choice_height(HWND choice, int face_height) {
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(choice,MONITOR_DEFAULTTONEAREST),&monitor);
    const int available = dip(choice,monitor.rcWork.bottom-monitor.rcWork.top);
    const auto count = std::max(1,int(SendMessageW(choice,CB_GETCOUNT,0,0)));
    return face_height + std::min(count*row_height+2*padding+4,std::max(row_height,available-face_height-16));
}
} // namespace cliprope::win::fluent
