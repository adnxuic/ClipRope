#include "platform_win/fluent_scroll.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <commctrl.h>
#include <ole2.h>
#include <wrl.h>
#include <uiautomation.h>
#include <windowsx.h>

namespace cliprope::win::fluent {
namespace {
constexpr wchar_t scroll_class[] = L"ClipRope.ScrollBar";
enum : UINT { read_position = WM_APP + 0x460, read_maximum, read_page, read_line, set_position, move_position };

class ScrollProvider final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
    IRawElementProviderSimple, IScrollProvider, IRangeValueProvider> {
  public:
    ScrollProvider(HWND host, HWND bar) : host_(host), bar_(bar), range_(host == bar) {}
    void disconnect() { host_ = nullptr; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* options) override {
        *options = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** provider) override {
        return UiaHostProviderFromHwnd(host_.load(), provider);
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT* value) override {
        VariantInit(value);
        if (id == UIA_NamePropertyId) {
            if (!host_) return UIA_E_ELEMENTNOTAVAILABLE;
            value->vt = VT_BSTR;
            value->bstrVal = SysAllocString(caption(host_.load()).c_str());
            return value->bstrVal ? S_OK : E_OUTOFMEMORY;
        }
        if (range_ && id == UIA_ControlTypePropertyId) {
            value->vt = VT_I4;
            value->lVal = UIA_ScrollBarControlTypeId;
        } else if (range_ && id == UIA_IsContentElementPropertyId) {
            value->vt = VT_BOOL;
            value->boolVal = VARIANT_FALSE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown** out) override {
        *out = nullptr;
        if (!range_ && id == UIA_ScrollPatternId) return QueryInterface(IID_IScrollProvider, reinterpret_cast<void**>(out));
        if (range_ && id == UIA_RangeValuePatternId) return QueryInterface(IID_IRangeValueProvider, reinterpret_cast<void**>(out));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Scroll(ScrollAmount horizontal, ScrollAmount vertical) override {
        if (!host_) return UIA_E_ELEMENTNOTAVAILABLE;
        if (horizontal != ScrollAmount_NoAmount) return UIA_E_INVALIDOPERATION;
        return static_cast<HRESULT>(SendMessageW(bar_, move_position, vertical, 0));
    }
    HRESULT STDMETHODCALLTYPE SetScrollPercent(double horizontal, double vertical) override {
        if (!host_) return UIA_E_ELEMENTNOTAVAILABLE;
        if (horizontal != UIA_ScrollPatternNoScroll) return UIA_E_INVALIDOPERATION;
        if (vertical == UIA_ScrollPatternNoScroll) return S_OK;
        if (!std::isfinite(vertical) || vertical < 0 || vertical > 100) return E_INVALIDARG;
        const int maximum = read(read_maximum);
        if (!maximum) return UIA_E_INVALIDOPERATION;
        return SetValue(std::round(maximum * vertical / 100.0));
    }
    HRESULT STDMETHODCALLTYPE get_HorizontallyScrollable(BOOL* v) override { *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_HorizontalScrollPercent(double* v) override { *v = UIA_ScrollPatternNoScroll; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_HorizontalViewSize(double* v) override { *v = 100; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_VerticallyScrollable(BOOL* v) override { *v = read(read_maximum) > 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_VerticalScrollPercent(double* v) override {
        const int maximum = read(read_maximum);
        *v = maximum ? 100.0 * read(read_position) / maximum : UIA_ScrollPatternNoScroll;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_VerticalViewSize(double* v) override {
        const int page = std::max(1, read(read_page));
        *v = 100.0 * page / (double(page) + read(read_maximum));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetValue(double v) override {
        if (!host_) return UIA_E_ELEMENTNOTAVAILABLE;
        if (!std::isfinite(v) || v < 0 || v > read(read_maximum)) return E_INVALIDARG;
        SendMessageW(bar_, set_position, WPARAM(std::lround(v)), 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Value(double* v) override { *v = read(read_position); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* v) override { *v = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Minimum(double* v) override { *v = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE get_Maximum(double* v) override { *v = read(read_maximum); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_LargeChange(double* v) override { *v = read(read_page); return S_OK; }
    HRESULT STDMETHODCALLTYPE get_SmallChange(double* v) override { *v = read(read_line); return S_OK; }
  private:
    int read(UINT msg) const { return host_ ? static_cast<int>(SendMessageW(bar_, msg, 0, 0)) : 0; }
    std::atomic<HWND> host_;
    HWND bar_;
    bool range_;
};
LRESULT CALLBACK accessibility(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto* provider = reinterpret_cast<ScrollProvider*>(data);
    if (msg == WM_GETOBJECT && static_cast<LONG>(l) == UiaRootObjectId)
        return UiaReturnRawElementProvider(h, w, l, provider);
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(h, accessibility, id);
        provider->disconnect();
        UiaDisconnectProvider(provider);
        provider->Release();
    }
    return DefSubclassProc(h, msg, w, l);
}
bool expose(HWND host, HWND bar) {
    auto provider = Microsoft::WRL::Make<ScrollProvider>(host, bar);
    if (!provider || !SetWindowSubclass(host, accessibility, 1, DWORD_PTR(provider.Get()))) return false;
    provider.Detach(); // The window subclass releases this reference at destruction.
    return true;
}
} // namespace

void announce_in_viewport(HWND viewport, std::wstring_view action, const std::wstring& result) {
    DWORD_PTR data{};
    if (!UiaClientsAreListening() || !GetWindowSubclass(viewport, accessibility, 1, &data)) return;
    const std::wstring message = std::wstring(action) + L". " + result;
    BSTR text = SysAllocString(message.c_str());
    BSTR activity = SysAllocStringLen(action.data(), UINT(action.size()));
    if (text && activity)
        UiaRaiseNotificationEvent(reinterpret_cast<ScrollProvider*>(data), NotificationKind_ActionCompleted,
                                  NotificationProcessing_MostRecent, text, activity);
    SysFreeString(activity);
    SysFreeString(text);
}

bool ScrollBar::create(HWND parent, HWND viewport, const Palette& colors, bool panel, std::function<void(int)> scroll) {
    colors_ = &colors;
    panel_ = panel;
    scroll_ = std::move(scroll);
    position_ = maximum_ = wheel_remainder_ = 0;
    grab_ = -1;
    hot_ = false;
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.lpszClassName = scroll_class;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    return create_ui_window(0, scroll_class, tr(TextKey::common_vertical_scrollbar), WS_CHILD | WS_TABSTOP, 0, 0, 1, 1,
                           parent, nullptr, wc.hInstance, this) && expose(window_, window_) &&
           (!viewport || expose(viewport, window_));
}
void ScrollBar::update(int position, int maximum, int page, int line) {
    maximum = std::max(0, maximum);
    page = std::max(1, page);
    position = std::clamp(position, 0, maximum);
    const bool changed = maximum != maximum_ || position != position_ || page != page_;
    maximum_ = maximum;
    position_ = position;
    page_ = page;
    line_ = std::max(1, line);
    if (!window_) return;
    if (bool(GetWindowLongW(window_, GWL_STYLE) & WS_VISIBLE) != bool(maximum))
        ShowWindow(window_, maximum ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (changed) {
        InvalidateRect(window_, nullptr, FALSE);
        NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, window_, OBJID_CLIENT, CHILDID_SELF);
    }
}
RECT ScrollBar::thumb() const {
    RECT r{};
    GetClientRect(window_, &r);
    const int height = r.bottom;
    const int size = std::min(height, std::max(px(window_, 24), int(std::round(height * (double(page_) / (double(maximum_) + page_))))));
    const int start = maximum_ ? MulDiv(height - size, position_, maximum_) : 0;
    const int half = px(window_, hot_ || grab_ >= 0 || GetFocus() == window_ ? 3 : 2);
    return {r.right / 2 - half, start, r.right / 2 + half, start + size};
}
void ScrollBar::paint(HDC dc) const {
    RECT r{};
    GetClientRect(window_, &r);
    solid(dc, r, panel_ ? colors_->panel : colors_->window);
    const auto t = thumb();
    const auto color = GetFocus() == window_ || grab_ >= 0 ? colors_->accent : colors_->muted;
    round_box(dc, t, (t.right - t.left) / 2, color, color, panel_ ? colors_->panel : colors_->window);
}
void ScrollBar::request(int next) {
    next = std::clamp(next, 0, maximum_);
    if (next != position_) scroll_(next);
}
void ScrollBar::wheel(WPARAM input) {
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    const int distance = lines == WHEEL_PAGESCROLL ? page_ : int(std::min(lines, 100u)) * line_;
    const long long scaled = wheel_remainder_ - static_cast<long long>(GET_WHEEL_DELTA_WPARAM(input)) * distance;
    wheel_remainder_ = int(scaled % WHEEL_DELTA);
    request(position_ + int(scaled / WHEEL_DELTA));
}
bool ScrollBar::key(WPARAM key) {
    switch (key) {
    case VK_UP: request(position_ - line_); break;
    case VK_DOWN: request(position_ + line_); break;
    case VK_PRIOR: request(position_ - page_); break;
    case VK_NEXT: request(position_ + page_); break;
    case VK_HOME: request(0); break;
    case VK_END: request(maximum_); break;
    default: return false;
    }
    return true;
}
LRESULT CALLBACK ScrollBar::proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<ScrollBar*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<ScrollBar*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->window_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(self));
    }
    if (!self) return DefWindowProcW(h, msg, w, l);
    switch (msg) {
    case read_position: return self->position_;
    case read_maximum: return self->maximum_;
    case read_page: return self->page_;
    case read_line: return self->line_;
    case set_position: self->request(int(w)); return 0;
    case move_position:
        if (!self->maximum_) return UIA_E_INVALIDOPERATION;
        switch (static_cast<ScrollAmount>(w)) {
        case ScrollAmount_LargeIncrement: self->key(VK_NEXT); break;
        case ScrollAmount_LargeDecrement: self->key(VK_PRIOR); break;
        case ScrollAmount_SmallIncrement: self->key(VK_DOWN); break;
        case ScrollAmount_SmallDecrement: self->key(VK_UP); break;
        case ScrollAmount_NoAmount: break;
        default: return E_INVALIDARG;
        }
        return S_OK;
    case WM_ERASEBKGND: return 1;
    case WM_PRINTCLIENT: self->paint(HDC(w)); return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        self->paint(BeginPaint(h, &ps));
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const auto t = self->thumb();
        const int y = GET_Y_LPARAM(l);
        if (y < t.top || y >= t.bottom) self->key(y < t.top ? VK_PRIOR : VK_NEXT);
        else {
            self->grab_ = y - t.top;
            SetCapture(h);
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (self->grab_ >= 0) {
            RECT r{};
            GetClientRect(h, &r);
            const auto t = self->thumb();
            const int travel = r.bottom - (t.bottom - t.top);
            if (travel > 0)
                self->request(MulDiv(std::clamp(GET_Y_LPARAM(l) - self->grab_, 0, travel), self->maximum_, travel));
        } else if (!self->hot_) {
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h};
            self->hot_ = TrackMouseEvent(&track) != FALSE;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSELEAVE: self->hot_ = false; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONUP:
    case WM_CANCELMODE:
        if (GetCapture() == h) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED: self->grab_ = -1; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_MOUSEWHEEL: self->wheel(w); return 0;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_KEYDOWN: if (self->key(w)) return 0; break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: InvalidateRect(h, nullptr, FALSE); break;
    case WM_NCDESTROY: self->window_ = nullptr; break;
    }
    return DefWindowProcW(h, msg, w, l);
}
} // namespace cliprope::win::fluent
