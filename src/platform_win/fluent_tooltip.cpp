#include "platform_win/fluent_tooltip.hpp"
#include "platform_win/card_menu_frame.hpp"
#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <optional>
#include <ole2.h>
#include <uiautomation.h>
#include <windowsx.h>
#include <wrl.h>

namespace cliprope::win::fluent {
namespace {
class TipProvider final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IRawElementProviderSimple> {
  public:
    explicit TipProvider(HWND window) : window_(window) {}
    void disconnect() { window_ = nullptr; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
        *out = ProviderOptions_ServerSideProvider; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        const auto h = window_.load();
        return h ? UiaHostProviderFromHwnd(h,out) : UIA_E_ELEMENTNOTAVAILABLE;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID, IUnknown** out) override { *out = nullptr; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* out) override {
        VariantInit(out);
        const auto h = window_.load();
        if (!h) return UIA_E_ELEMENTNOTAVAILABLE;
        if (id == UIA_ControlTypePropertyId) { out->vt=VT_I4; out->lVal=UIA_ToolTipControlTypeId; }
        if (id == UIA_NamePropertyId) {
            out->vt=VT_BSTR; out->bstrVal=SysAllocString(caption(h).c_str());
            if (!out->bstrVal) return E_OUTOFMEMORY;
        }
        if (id == UIA_IsKeyboardFocusablePropertyId || id == UIA_IsOffscreenPropertyId ||
            id == UIA_IsContentElementPropertyId || id == UIA_IsControlElementPropertyId) {
            out->vt=VT_BOOL;
            out->boolVal = (id == UIA_IsContentElementPropertyId || id == UIA_IsControlElementPropertyId ||
                           (id == UIA_IsOffscreenPropertyId && !IsWindowVisible(h))) ? VARIANT_TRUE : VARIANT_FALSE;
        }
        return S_OK;
    }
  private:
    std::atomic<HWND> window_;
};
}
struct Tooltip::Impl {
    struct Binding { HWND target; std::function<UiText()> text; int width; std::optional<RECT> area; bool keyboard; std::optional<POINT> pointer; };
    std::vector<Binding> bindings;
    HWND owner{}, popup{}, target{}, foreground{};
    Surface surface;
    CardMenuFrame frame;
    Microsoft::WRL::ComPtr<TipProvider> provider;
    UiText message;
    int width{320};
    bool keyboard{};

    ~Impl() { clear(); }
    Binding* binding(HWND h) {
        const auto it=std::find_if(bindings.begin(),bindings.end(),[&](const auto& b) { return b.target==h; });
        return it==bindings.end() ? nullptr : &*it;
    }
    bool inside(const Binding& b) const {
        POINT point{}; RECT area{};
        if (GetCursorPos(&point)) {
            if (!ScreenToClient(b.target,&point)) return false;
        } else if (b.pointer) {
            // Input-desktop access can be unavailable; a delivered move remains valid until leave.
            point=*b.pointer;
        } else return false;
        if (b.area) area=*b.area; else GetClientRect(b.target,&area);
        return PtInRect(&area,point) != FALSE;
    }
    bool create(HWND parent) {
        if (popup && owner==parent) return true;
        clear();
        owner=parent;
        WNDCLASSW wc{};
        wc.hInstance=GetModuleHandleW(nullptr); wc.lpfnWndProc=proc; wc.lpszClassName=L"ClipRope.Tooltip";
        if (!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
        popup=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TRANSPARENT,
            wc.lpszClassName,L"",WS_POPUP,0,0,1,1,parent,nullptr,wc.hInstance,this);
        if (!popup) return false;
        surface.attach(popup);
        provider=Microsoft::WRL::Make<TipProvider>(popup);
        SetWindowSubclass(owner,owner_proc,UINT_PTR(this),DWORD_PTR(this));
        return true;
    }
    void hide() {
        if (popup) {
            KillTimer(popup,1);
            if (IsWindowVisible(popup)) {
                ShowWindow(popup,SW_HIDE);
                if (provider && UiaClientsAreListening()) UiaRaiseAutomationEvent(provider.Get(),UIA_ToolTipClosedEventId);
            }
        }
        if (frame.window()) ShowWindow(frame.window(),SW_HIDE);
        target=nullptr;
    }
    void clear() {
        hide();
        for (const auto& b:bindings) RemoveWindowSubclass(b.target,source_proc,UINT_PTR(this));
        bindings.clear();
        if (owner) RemoveWindowSubclass(owner,owner_proc,UINT_PTR(this));
        if (popup) DestroyWindow(popup);
        frame.close();
        popup=owner=nullptr;
        message={};
    }
    void request(Binding& b,bool immediate) {
        if (GetCapture() || !IsWindowVisible(b.target) || !IsWindowVisible(owner)) { hide(); return; }
        if (target==b.target && !immediate) return;
        hide();
        target=b.target; foreground=GetForegroundWindow(); keyboard=immediate;
        message=b.text(); width=b.width;
        if (message.empty()) { target=nullptr; return; }
        if (immediate) show();
        else {
            // A tracked target can be bound during its first move notification.
            TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,target,0};
            TrackMouseEvent(&tracking);
            SetTimer(popup,1,500,nullptr);
        }
    }
    void show() {
        auto* b=binding(target);
        if (!b || !IsWindowVisible(target) || !IsWindowVisible(owner) || GetCapture() ||
            GetForegroundWindow()!=foreground || (!keyboard && !inside(*b))) { hide(); return; }
        message=b->text();
        if (message.empty()) { hide(); return; }
        set_ui_text(popup,message);
        accessible_description(target,message);
        RECT anchor{}; GetWindowRect(target,&anchor);
        if (b->area) { anchor=*b->area; MapWindowPoints(target,nullptr,reinterpret_cast<POINT*>(&anchor),2); }
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromRect(&anchor,MONITOR_DEFAULTTONEAREST),&monitor);
        const auto work=monitor.rcWork;
        const int inset=px(popup,10);
        const int max_width=std::max(1,std::min(px(popup,width),int(work.right-work.left)-2*inset));
        const auto measured=surface.measure_paragraph(message,max_width);
        const int w=std::min<int>(work.right-work.left,measured.cx+2*inset);
        const int h=std::min<int>(work.bottom-work.top,measured.cy+2*inset);
        int x=std::clamp<int>(anchor.left,work.left,work.right-w), y=anchor.bottom+px(popup,6);
        if (y+h>work.bottom) y=anchor.top-h-px(popup,6);
        y=std::clamp<int>(y,work.top,work.bottom-h);
        SetWindowPos(popup,HWND_TOPMOST,x,y,w,h,SWP_NOACTIVATE);
        if (!frame.window()) frame.open(owner);
        if (frame.window()) {
            frame.fit(popup,surface.colors,design_dpi(GetDpiForWindow(popup)),8);
            SetWindowPos(frame.window(),popup,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
        }
        ShowWindow(popup,SW_SHOWNOACTIVATE);
        InvalidateRect(popup,nullptr,FALSE);
        if (provider && UiaClientsAreListening()) UiaRaiseAutomationEvent(provider.Get(),UIA_ToolTipOpenedEventId);
    }
    void paint(HDC dc) {
        RECT r{}; GetClientRect(popup,&r);
        round_box(dc,r,px(popup,8),surface.colors.panel,surface.colors.line);
        InflateRect(&r,-px(popup,10),-px(popup,10));
        surface.draw_paragraph(dc,r,message,surface.colors.text,surface.colors.panel);
    }
    static LRESULT CALLBACK source_proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR id,DWORD_PTR data) {
        auto& self=*reinterpret_cast<Impl*>(data);
        if (auto* b=self.binding(h)) {
            if (m==WM_MOUSEMOVE) b->pointer=POINT{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
            if (m==WM_MOUSELEAVE) b->pointer.reset();
            if (m==WM_MOUSEMOVE && self.inside(*b)) {
                TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,h,0}; TrackMouseEvent(&tracking);
                self.request(*b,false);
            } else if (m==WM_SETFOCUS && b->keyboard) self.request(*b,true);
            else if ((m==WM_MOUSEMOVE && !self.inside(*b)) || m==WM_MOUSELEAVE || m==WM_KILLFOCUS ||
                     m==WM_MOUSEWHEEL || m==WM_LBUTTONDOWN || m==WM_RBUTTONDOWN || m==WM_CAPTURECHANGED ||
                     m==WM_WINDOWPOSCHANGING || (m==WM_KEYDOWN && w==VK_ESCAPE)) self.hide();
        }
        if (m==WM_NCDESTROY) {
            if (self.target==h) self.hide();
            std::erase_if(self.bindings,[&](const auto& b) { return b.target==h; });
            RemoveWindowSubclass(h,source_proc,id);
        }
        return DefSubclassProc(h,m,w,l);
    }
    static LRESULT CALLBACK owner_proc(HWND h,UINT m,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR data) {
        auto& self=*reinterpret_cast<Impl*>(data);
        if (m==WM_NCDESTROY) self.clear();
        else if (m==WM_MOUSEWHEEL || m==WM_SIZE || m==WM_MOVE || m==WM_CANCELMODE ||
                 (m==WM_ACTIVATE && LOWORD(w)==WA_INACTIVE) || (m==WM_SHOWWINDOW && !w)) self.hide();
        return DefSubclassProc(h,m,w,l);
    }
    static LRESULT CALLBACK proc(HWND h,UINT m,WPARAM w,LPARAM l) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if (m==WM_NCCREATE) {
            self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(h,GWLP_USERDATA,LONG_PTR(self));
        }
        if (!self) return DefWindowProcW(h,m,w,l);
        switch (m) {
        case WM_TIMER: if (w==1) { KillTimer(h,1); self->show(); return 0; } break;
        case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
        case WM_NCHITTEST: return HTTRANSPARENT;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps{}; self->paint(BeginPaint(h,&ps)); EndPaint(h,&ps); return 0; }
        case WM_PRINTCLIENT: self->paint(HDC(w)); return 0;
        case WM_GETOBJECT:
            if (static_cast<LONG>(l)==UiaRootObjectId && self->provider)
                return UiaReturnRawElementProvider(h,w,l,self->provider.Get());
            break;
        case WM_THEMECHANGED: case WM_SETTINGCHANGE: case WM_SYSCOLORCHANGE: case language_changed:
            self->surface.refresh(); if (IsWindowVisible(h)) self->show(); return 0;
        case WM_DPICHANGED: self->surface.refresh(); if (IsWindowVisible(h)) self->show(); return 0;
        case WM_NCDESTROY:
            if (self->provider) {
                self->provider->disconnect(); UiaDisconnectProvider(self->provider.Get()); self->provider.Reset();
            }
            self->popup=nullptr; self->surface.release(); break;
        }
        return DefWindowProcW(h,m,w,l);
    }
};
Tooltip::Tooltip() : impl_(std::make_unique<Impl>()) {}
Tooltip::~Tooltip() = default;
void Tooltip::bind(HWND owner,HWND target,std::function<UiText()> text,int width) {
    if (!impl_->create(owner)) return;
    if (auto* b=impl_->binding(target)) { b->text=std::move(text); b->width=width; return; }
    if (SetWindowSubclass(target,Impl::source_proc,UINT_PTR(impl_.get()),DWORD_PTR(impl_.get())))
        impl_->bindings.push_back({target,std::move(text),width,{},true});
}
void Tooltip::track(HWND owner,HWND target,UiText text,bool immediate,int width,const RECT* area) {
    bind(owner,target,[text] { return text; },width);
    auto* b=impl_->binding(target);
    if (!b) return;
    b->area=area ? std::optional<RECT>(*area) : std::nullopt;
    b->keyboard=false;
    if (immediate || impl_->inside(*b)) impl_->request(*b,immediate);
}
void Tooltip::hide() { impl_->hide(); }
void Tooltip::clear() { impl_->clear(); }
bool Tooltip::visible() const { return impl_->popup && IsWindowVisible(impl_->popup); }
HWND Tooltip::window() const { return impl_->popup; }
} // namespace cliprope::win::fluent
