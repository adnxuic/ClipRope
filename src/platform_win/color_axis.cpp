#include "platform_win/color_axis.hpp"
#include <atomic>
#include <d2d1.h>
#include <ole2.h>
#include <uiautomation.h>
#include <windowsx.h>
#include <wrl.h>

namespace cliprope::win {
namespace {
using Microsoft::WRL::ComPtr;
constexpr UINT read_axis = WM_APP + 0x510, write_axis = WM_APP + 0x511;
struct Values {
    double x{}, y{}, maximum{};
};
class AxisProvider final
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
                                          IRawElementProviderSimple, IRangeValueProvider, IValueProvider> {
    std::atomic<HWND> window_;
    bool sv_;
    Values read() const {
        Values v;
        if (window_)
            SendMessageW(window_, read_axis, 0, LPARAM(&v));
        return v;
    }

  public:
    AxisProvider(HWND h, bool sv) : window_(h), sv_(sv) {}
    void disconnect() { window_ = nullptr; }
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions *p) override {
        *p = ProviderOptions_ServerSideProvider;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple **p) override {
        return UiaHostProviderFromHwnd(window_, p);
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID id, IUnknown **p) override {
        *p = nullptr;
        if (id == UIA_ValuePatternId && sv_)
            return QueryInterface(IID_IValueProvider, reinterpret_cast<void **>(p));
        if (id == UIA_RangeValuePatternId && !sv_)
            return QueryInterface(IID_IRangeValueProvider, reinterpret_cast<void **>(p));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID id, VARIANT *v) override {
        VariantInit(v);
        if (!window_)
            return UIA_E_ELEMENTNOTAVAILABLE;
        if (id == UIA_NamePropertyId) {
            v->vt = VT_BSTR;
            v->bstrVal = SysAllocString(fluent::caption(window_).c_str());
        } else if (id == UIA_ControlTypePropertyId) {
            v->vt = VT_I4;
            v->lVal = sv_ ? UIA_CustomControlTypeId : UIA_SliderControlTypeId;
        } else if (id == UIA_IsKeyboardFocusablePropertyId) {
            v->vt = VT_BOOL;
            v->boolVal = VARIANT_TRUE;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetValue(double scalar) override {
        if (!window_)
            return UIA_E_ELEMENTNOTAVAILABLE;
        auto v = read();
        if (!std::isfinite(scalar) || scalar < 0 || scalar > v.maximum)
            return E_INVALIDARG;
        v.x = scalar;
        SendMessageW(window_, write_axis, 0, LPARAM(&v));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Value(double *scalar) override {
        *scalar = read().x;
        return window_ ? S_OK : UIA_E_ELEMENTNOTAVAILABLE;
    }
    HRESULT STDMETHODCALLTYPE get_Minimum(double *scalar) override {
        *scalar = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Maximum(double *scalar) override {
        *scalar = read().maximum;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_LargeChange(double *scalar) override {
        *scalar = 10;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SmallChange(double *scalar) override {
        *scalar = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL *scalar) override {
        *scalar = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR text) override {
        if (!window_)
            return UIA_E_ELEMENTNOTAVAILABLE;
        double s{}, v{};
        wchar_t extra{};
        if (swscanf_s(text, L"%lf , %lf %lc", &s, &v, &extra, 1) != 2 || !std::isfinite(s) ||
            !std::isfinite(v) || s < 0 || s > 100 || v < 0 || v > 100)
            return E_INVALIDARG;
        Values values{s, v, 100};
        SendMessageW(window_, write_axis, 0, LPARAM(&values));
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR *text) override {
        auto v = read();
        wchar_t scalar[64];
        swprintf_s(scalar, L"%.1f, %.1f", v.x, v.y);
        *text = SysAllocString(scalar);
        return *text ? S_OK : E_OUTOFMEMORY;
    }
};
struct Graphics {
    ComPtr<ID2D1Factory> factory;
    ComPtr<ID2D1DCRenderTarget> target;
    ComPtr<ID2D1SolidColorBrush> ink;
    ComPtr<ID2D1LinearGradientBrush> spectrum, shade, hue, alpha;
    ComPtr<ID2D1BitmapBrush> checks;
    double cached_hue = -1;
    unsigned cached_alpha = 0xffffffff;
    bool bind(HDC dc, RECT bounds) {
        if (!factory && FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())))
            return false;
        if (!target) {
            auto p = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
            if (FAILED(factory->CreateDCRenderTarget(&p, &target)))
                return false;
            target->CreateSolidColorBrush(D2D1::ColorF(0), &ink);
        }
        return SUCCEEDED(target->BindDC(dc, &bounds));
    }
    void gradient(ComPtr<ID2D1LinearGradientBrush> &brush, const D2D1_GRADIENT_STOP *stops, UINT count) {
        ComPtr<ID2D1GradientStopCollection> collection;
        if (SUCCEEDED(target->CreateGradientStopCollection(stops, count, D2D1_GAMMA_2_2,
                                                           D2D1_EXTEND_MODE_CLAMP, &collection)))
            target->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({0, 0}, {1, 0}),
                                              collection.Get(), brush.ReleaseAndGetAddressOf());
    }
    void reset() {
        ink.Reset();
        spectrum.Reset();
        shade.Reset();
        hue.Reset();
        alpha.Reset();
        checks.Reset();
        target.Reset();
        cached_hue = -1;
        cached_alpha = 0xffffffff;
    }
};
thread_local Graphics graphics;
struct Axis {
    ColorAxis kind;
    domain::ColorHSV hsv;
    domain::ColorRGBA8 color;
    std::function<void(double, double)> changed;
    ComPtr<AxisProvider> provider;
    HWND window{};
    bool dragging{};
    Values values() const {
        return kind == ColorAxis::sv    ? Values{hsv.s * 100, hsv.v * 100, 100}
               : kind == ColorAxis::hue ? Values{hsv.h, 0, 360}
                                        : Values{color.a * 100. / 255, 0, 100};
    }
    void set(Values value) { changed(std::clamp(value.x, 0., value.maximum), std::clamp(value.y, 0., 100.)); }
    void pointer(int x, int y) {
        RECT r;
        GetClientRect(window, &r);
        auto v = values();
        const int inset = kind == ColorAxis::sv ? 0 : fluent::px(window, 15);
        v.x = double(x - inset) / std::max(1L, r.right - 2 * inset) * v.maximum;
        if (kind == ColorAxis::sv)
            v.y = (1 - double(y) / std::max(1L, r.bottom)) * 100;
        set(v);
    }
    void paint(HDC dc, RECT bounds) {
        auto &g = graphics;
        if (!g.bind(dc, bounds))
            return;
        float w = float(bounds.right), h = float(bounds.bottom),
              scale = fluent::design_dpi(GetDpiForWindow(window)) / 96.f;
        g.target->BeginDraw();
        auto palette = fluent::system_palette();
        auto panel = palette.panel;
        g.target->Clear(
            D2D1::ColorF(GetRValue(panel) / 255.f, GetGValue(panel) / 255.f, GetBValue(panel) / 255.f));
        auto rect = D2D1::RectF(0, 0, w, h);
        auto colors = [](domain::ColorRGBA8 c) {
            return D2D1::ColorF(c.r / 255.f, c.g / 255.f, c.b / 255.f, c.a / 255.f);
        };
        if (kind == ColorAxis::sv) {
            if (!g.spectrum || g.cached_hue != hsv.h) {
                D2D1_GRADIENT_STOP stops[]{{0, D2D1::ColorF(0xffffff)},
                                           {1, colors(domain::ColorHSV{hsv.h, 1, 1}.rgba())}};
                g.gradient(g.spectrum, stops, 2);
                g.cached_hue = hsv.h;
            }
            if (!g.shade) {
                D2D1_GRADIENT_STOP stops[]{{0, D2D1::ColorF(0, 0.f)}, {1, D2D1::ColorF(0, 1.f)}};
                g.gradient(g.shade, stops, 2);
            }
            if (g.spectrum && g.shade) {
                g.spectrum->SetEndPoint({w, 0});
                g.shade->SetEndPoint({0, h});
                const auto field = D2D1::RoundedRect(rect, 6 * scale, 6 * scale);
                g.target->FillRoundedRectangle(field, g.spectrum.Get());
                g.target->FillRoundedRectangle(field, g.shade.Get());
                const auto edge = palette.line;
                g.ink->SetColor(
                    D2D1::ColorF(GetRValue(edge) / 255.f, GetGValue(edge) / 255.f, GetBValue(edge) / 255.f));
                g.target->DrawRoundedRectangle(field, g.ink.Get(), .7f * scale);
            }
        } else {
            rect = {7 * scale, (h - 11 * scale) / 2, w - 7 * scale, (h + 11 * scale) / 2};
            ID2D1LinearGradientBrush *bar{};
            if (kind == ColorAxis::hue) {
                if (!g.hue) {
                    D2D1_GRADIENT_STOP stops[7];
                    for (int i = 0; i < 7; ++i)
                        stops[i] = {i / 6.f, colors(domain::ColorHSV{double(i * 60), 1, 1}.rgba())};
                    g.gradient(g.hue, stops, 7);
                }
                bar = g.hue.Get();
            } else {
                if (!g.checks) {
                    unsigned pixels[64];
                    for (int y = 0; y < 8; ++y)
                        for (int x = 0; x < 8; ++x)
                            pixels[y * 8 + x] = ((x / 4 + y / 4) % 2) ? 0xffdedbd6 : 0xfff8f6f2;
                    ComPtr<ID2D1Bitmap> bitmap;
                    auto props = D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                    if (SUCCEEDED(g.target->CreateBitmap({8, 8}, pixels, 32, props, &bitmap)))
                        g.target->CreateBitmapBrush(
                            bitmap.Get(),
                            D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP),
                            &g.checks);
                }
                if (g.checks)
                    g.target->FillRoundedRectangle(D2D1::RoundedRect(rect, 5.5f * scale, 5.5f * scale),
                                                   g.checks.Get());
                if (!g.alpha || g.cached_alpha != color.rgb()) {
                    auto a = color;
                    a.a = 0;
                    auto b = color;
                    b.a = 255;
                    D2D1_GRADIENT_STOP stops[]{{0, colors(a)}, {1, colors(b)}};
                    g.gradient(g.alpha, stops, 2);
                    g.cached_alpha = color.rgb();
                }
                bar = g.alpha.Get();
            }
            if (bar) {
                bar->SetStartPoint({rect.left, 0});
                bar->SetEndPoint({rect.right, 0});
                g.target->FillRoundedRectangle(D2D1::RoundedRect(rect, 5.5f * scale, 5.5f * scale), bar);
            }
        }
        auto v = values();
        float x = kind == ColorAxis::sv ? std::clamp(float(hsv.s) * w, 9 * scale, w - 9 * scale)
                                        : rect.left + 8 * scale +
                                              float(v.x / v.maximum) * (rect.right - rect.left - 16 * scale);
        float y = kind == ColorAxis::sv ? std::clamp(float(1 - hsv.v) * h, 9 * scale, h - 9 * scale) : h / 2;
        g.ink->SetColor(D2D1::ColorF(0, 0.22f));
        g.target->FillEllipse(D2D1::Ellipse({x, y + scale}, 8.5f * scale, 8.5f * scale), g.ink.Get());
        auto fill = kind == ColorAxis::hue ? domain::ColorHSV{hsv.h, 1, 1}.rgba() : color;
        fill.a = 255;
        g.ink->SetColor(colors(fill));
        g.target->FillEllipse(D2D1::Ellipse({x, y}, 7.5f * scale, 7.5f * scale), g.ink.Get());
        g.ink->SetColor(D2D1::ColorF(0xffffff));
        g.target->DrawEllipse(D2D1::Ellipse({x, y}, 7.5f * scale, 7.5f * scale), g.ink.Get(), 1.8f * scale);
        if (GetFocus() == window) {
            auto c = palette.accent;
            g.ink->SetColor(D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f));
            g.target->DrawRectangle(D2D1::RectF(1, 1, w - 1, h - 1), g.ink.Get(), scale);
        }
        if (FAILED(g.target->EndDraw()))
            g.reset();
    }
};
LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto a = reinterpret_cast<Axis *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        a = static_cast<Axis *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
        a->window = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(a));
    }
    if (!a)
        return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_GETOBJECT:
        if (static_cast<LONG>(l) == UiaRootObjectId) {
            if (!a->provider)
                a->provider = Microsoft::WRL::Make<AxisProvider>(h, a->kind == ColorAxis::sv);
            return UiaReturnRawElementProvider(h, w, l, a->provider.Get());
        }
        break;
    case read_axis:
        *reinterpret_cast<Values *>(l) = a->values();
        return 0;
    case write_axis:
        a->set(*reinterpret_cast<Values *>(l));
        return 0;
    case WM_KEYDOWN: {
        auto v = a->values();
        double step = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
        if (w == VK_LEFT)
            v.x -= step;
        else if (w == VK_RIGHT)
            v.x += step;
        else if (w == VK_UP) {
            if (a->kind == ColorAxis::sv)
                v.y += step;
            else
                v.x += step;
        } else if (w == VK_DOWN) {
            if (a->kind == ColorAxis::sv)
                v.y -= step;
            else
                v.x -= step;
        } else if (w == VK_HOME)
            v.x = 0;
        else if (w == VK_END)
            v.x = v.maximum;
        else
            break;
        a->set(v);
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(h);
        SetCapture(h);
        a->dragging = true;
        a->pointer(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_MOUSEMOVE:
        if (a->dragging)
            a->pointer(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_LBUTTONUP:
        if (a->dragging)
            a->pointer(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        a->dragging = false;
        if (GetCapture() == h)
            ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        a->dragging = false;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        auto dc = BeginPaint(h, &ps);
        RECT r;
        GetClientRect(h, &r);
        a->paint(dc, r);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_NCDESTROY:
        if (a->provider) {
            a->provider->disconnect();
            UiaDisconnectProvider(a->provider.Get());
        }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        delete a;
        break;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace
HWND create_color_axis(HWND parent, int id, ColorAxis kind, UiText name,
                       std::function<void(double, double)> changed) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ClipRope.ColorAxis";
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;
    auto a = new Axis{kind, {}, {}, std::move(changed)};
    return create_ui_window(0, wc.lpszClassName, name, WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 1, 1, parent,
                            HMENU(INT_PTR(id)), wc.hInstance, a);
}
void update_color_axis(HWND control, domain::ColorHSV hsv, domain::ColorRGBA8 color) {
    auto a = reinterpret_cast<Axis *>(GetWindowLongPtrW(control, GWLP_USERDATA));
    if (!a)
        return;
    if (a->hsv.h == hsv.h && a->hsv.s == hsv.s && a->hsv.v == hsv.v && a->color == color)
        return;
    a->hsv = hsv;
    a->color = color;
    InvalidateRect(control, nullptr, FALSE);
    NotifyWinEvent(EVENT_OBJECT_VALUECHANGE, control, OBJID_CLIENT, CHILDID_SELF);
}
} // namespace cliprope::win
