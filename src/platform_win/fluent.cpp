#include "platform_win/fluent.hpp"
#include "platform_win/app_icon.hpp"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <d2d1.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <dwrite.h>
#include <dwrite_2.h>
#include <initguid.h>
#include <memory>
#include <list>
#include "platform_win/menu_bitmap.hpp"
#include <numbers>
#include <oleacc.h>
#include <optional>
#include <wrl/client.h>
#include <wincodec.h>

namespace cliprope::win::fluent {
namespace {
LRESULT CALLBACK button_notifications(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
thread_local application::Theme selected_theme = application::Theme::system;
}
void set_theme(application::Theme mode) {
    if (selected_theme == mode) return;
    selected_theme = mode;
    EnumThreadWindows(GetCurrentThreadId(), [](HWND h, LPARAM) -> BOOL {
        DWORD_PTR data{};
        if (GetWindowSubclass(h,button_notifications,1,&data)) SendMessageW(h, WM_SETTINGCHANGE, 0, 0);
        return TRUE;
    }, 0);
}
Palette system_palette() {
    HIGHCONTRASTW hc{sizeof(hc)};
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON)) {
        return {GetSysColor(COLOR_WINDOW),
                GetSysColor(COLOR_WINDOW),
                GetSysColor(COLOR_BTNFACE),
                GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_HIGHLIGHT),
                GetSysColor(COLOR_WINDOWTEXT),
                GetSysColor(COLOR_HIGHLIGHTTEXT),
                false,
                true};
    }
    DWORD light = 1, bytes = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes);
#ifdef CLIPROPE_DESKTOP_PROBES
    // The qualification executable can render the design matrix without changing Windows personalization.
    wchar_t preview[24]{};
    GetEnvironmentVariableW(L"CLIPROPE_PROBE_UI_THEME", preview, 24);
    if (wcscmp(preview, L"dark") == 0)
        light = 0;
    if (wcscmp(preview, L"light") == 0)
        light = 1;
    if (wcscmp(preview, L"contrast") == 0)
        return {RGB(0, 0, 0),
                RGB(0, 0, 0),
                RGB(0, 0, 0),
                RGB(255, 255, 255),
                RGB(255, 255, 255),
                RGB(255, 255, 255),
                RGB(255, 255, 0),
                RGB(255, 255, 255),
                RGB(0, 0, 0),
                true,
                true};
#endif
    if (selected_theme != application::Theme::system) light = selected_theme == application::Theme::light;
    if (!light)
        return {RGB(27, 27, 27),    RGB(41, 40, 39),    RGB(56, 54, 52),    RGB(81, 76, 71), RGB(242, 238, 232),
                RGB(189, 181, 172), RGB(214, 161, 109), RGB(255, 156, 148), RGB(35, 28, 22), true};
    return {RGB(245, 244, 240), RGB(255, 255, 255), RGB(240, 238, 233), RGB(231, 227, 218), RGB(36, 36, 36),
            RGB(113, 107, 100), RGB(166, 110, 61),  RGB(161, 48, 40),   RGB(255, 255, 255)};
}
COLORREF inset_surface(const Palette& p) {
    if (p.contrast) return p.subtle;
    return RGB((GetRValue(p.panel)+GetRValue(p.subtle))/2,
               (GetGValue(p.panel)+GetGValue(p.subtle))/2,
               (GetBValue(p.panel)+GetBValue(p.subtle))/2);
}
UINT design_dpi(UINT monitor_dpi) {
#ifdef CLIPROPE_DESKTOP_PROBES
    static const UINT override_dpi = [] {
        wchar_t value[8]{};
        GetEnvironmentVariableW(L"CLIPROPE_PROBE_UI_DPI", value, 8);
        const int dpi = _wtoi(value);
        return dpi == 96 || dpi == 120 || dpi == 144 || dpi == 192 ? UINT(dpi) : 0u;
    }();
    if (override_dpi)
        return override_dpi;
#endif
    return monitor_dpi ? monitor_dpi : 96;
}
int px(HWND h, int n) { return MulDiv(n, design_dpi(GetDpiForWindow(h)), 96); }
int dip(HWND h, int n) { return MulDiv(n, 96, design_dpi(GetDpiForWindow(h))); }
void place(HWND h, int x, int y, int w, int height) {
    if (h)
        MoveWindow(h, px(h, x), px(h, y), px(h, std::max(1, w)), px(h, std::max(1, height)), TRUE);
}
std::wstring caption(HWND h) {
    std::wstring value(GetWindowTextLengthW(h) + 1, L'\0');
    value.resize(GetWindowTextW(h, value.data(), int(value.size())));
    return value;
}
namespace {
// Opt controls into common-controls v6 without changing the process-wide manifest.
struct ButtonContext {
    ButtonContext() {
        ACTCTXW context{sizeof(context)};
        context.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
        context.hModule = GetModuleHandleW(nullptr);
        context.lpResourceName = L"CLIPROPE_FLUENT_BUTTONS";
        handle = CreateActCtxW(&context);
    }
    ~ButtonContext() { if (handle != INVALID_HANDLE_VALUE) ReleaseActCtx(handle); }
    HANDLE handle;
};
}
void ChildLayout::move(HWND child, int x, int y, int width, int height, UINT visibility) {
    if (child) moves_.push_back({child, x, y, std::max(1, width), std::max(1, height),
                                 SWP_NOACTIVATE | SWP_NOZORDER | visibility});
}
void ChildLayout::apply(HWND parent, bool repaint_children) {
    if (moves_.empty()) return;
    const UINT redraw = repaint_children ? SWP_NOREDRAW | SWP_NOCOPYBITS : 0;
    auto batch = BeginDeferWindowPos(static_cast<int>(moves_.size()));
    for (const auto& m : moves_) {
        if (!batch) break;
        batch = DeferWindowPos(batch, m.child, nullptr, m.x, m.y, m.width, m.height, m.flags | redraw);
    }
    if (!batch || !EndDeferWindowPos(batch))
        for (const auto& m : moves_)
            SetWindowPos(m.child, nullptr, m.x, m.y, m.width, m.height, m.flags | redraw);
    if (repaint_children)
        RedrawWindow(parent, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
    else
        InvalidateRect(parent, nullptr, FALSE);
}
void ChildLayout::place(HWND h, int x, int y, int w, int height, UINT visibility) {
    if (!h) return;
    const UINT dpi = design_dpi(GetDpiForWindow(h));
    const int left = MulDiv(x, dpi, 96), top = MulDiv(y, dpi, 96);
    move(h, left, top, MulDiv(x + w, dpi, 96) - left, MulDiv(y + height, dpi, 96) - top, visibility);
}
HWND child(HWND h, const wchar_t* cls, UiText text, DWORD style, int id, bool use_visual_styles) {
    ULONG_PTR cookie{};
    if (use_visual_styles || _wcsicmp(cls, L"BUTTON") == 0) {
        static const ButtonContext context;
        if (context.handle == INVALID_HANDLE_VALUE || !ActivateActCtx(context.handle, &cookie))
            return nullptr;
    }
    HWND control = CreateWindowExW(0, cls, text.c_str(), WS_VISIBLE | WS_CHILD | style, 0, 0, 1, 1, h,
                                   reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    if (cookie) DeactivateActCtx(0, cookie);
    if (control) bind_ui_text(control, std::move(text));
    return control;
}
void solid(HDC dc, RECT r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
void round_box(HDC dc, RECT r, int radius, COLORREF fill, COLORREF border, COLORREF backdrop) {
    if (Surface::rounded(dc, r, radius, fill, border, backdrop == CLR_INVALID ? fill : backdrop)) return;
    // Last-resort rendering remains usable if Direct2D cannot create a target.
    auto oldpen = SelectObject(dc, GetStockObject(DC_PEN));
    auto oldbrush = SelectObject(dc, GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, fill);
    SetDCPenColor(dc, border);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius * 2, radius * 2);
    SelectObject(dc, oldbrush);
    SelectObject(dc, oldpen);
}
void window_frame(HWND h, const Palette& p, bool compact_corner) {
    BOOL dark = p.dark;
    DwmSetWindowAttribute(h, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    auto corner = compact_corner ? DWMWCP_ROUNDSMALL : DWMWCP_ROUND;
    DwmSetWindowAttribute(h, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
}
namespace {
LRESULT CALLBACK edit_frame(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    auto& theme = *reinterpret_cast<Surface*>(data);
    const bool inset = GetPropW(h, L"ClipRope.InsetField") != nullptr;
    if (msg == WM_NCCALCSIZE) {
        auto* r = w ? &reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0] : reinterpret_cast<RECT*>(l);
        InflateRect(r, -px(h, inset ? 8 : 5), -px(h, inset ? 8 : 5));
        return 0;
    }
    if (msg == WM_NCPAINT) {
        auto dc = GetWindowDC(h);
        if (!dc)
            return 0;
        RECT r{};
        GetWindowRect(h, &r);
        OffsetRect(&r, -r.left, -r.top);
        RECT client = r;
        InflateRect(&client, -px(h, inset ? 8 : 5), -px(h, inset ? 8 : 5));
        ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
        theme.draw_card(h, dc, r, {inset ? CardPart::picker_field : CardPart::dialog_field,
                                   0, {}, {}, {}, 0, GetFocus() == h});
        ReleaseDC(h, dc);
        return 0;
    }
    if (msg == WM_NCDESTROY) {
        RemovePropW(h, L"ClipRope.InsetField");
        RemoveWindowSubclass(h, edit_frame, 1);
    }
    auto result = DefSubclassProc(h, msg, w, l);
    if (msg == WM_SETFOCUS || msg == WM_KILLFOCUS)
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
    return result;
}
LRESULT CALLBACK dialog_frame(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR) {
    switch (msg) {
    case WM_NCCALCSIZE:
        if (w && IsZoomed(h)) {
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &monitor))
                reinterpret_cast<NCCALCSIZE_PARAMS*>(l)->rgrc[0] = monitor.rcWork;
        }
        return 0;
    case WM_NCHITTEST: {
        RECT area{};
        GetWindowRect(h, &area);
        const POINT pt{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        const int edge = px(h, 6);
        if (!IsZoomed(h) && (GetWindowLongPtrW(h, GWL_STYLE) & WS_THICKFRAME)) {
            const bool left = pt.x < area.left + edge, right = pt.x >= area.right - edge;
            if (pt.y < area.top + edge) return left ? HTTOPLEFT : right ? HTTOPRIGHT : HTTOP;
            if (pt.y >= area.bottom - edge) return left ? HTBOTTOMLEFT : right ? HTBOTTOMRIGHT : HTBOTTOM;
            if (left || right) return left ? HTLEFT : HTRIGHT;
        }
        RECT close{};
        GetWindowRect(GetDlgItem(h, dialog_close_id), &close);
        if (!PtInRect(&close, pt) && pt.y < area.top + px(h, 33)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_COMMAND:
        if (LOWORD(w) == dialog_close_id) { SendMessageW(h, WM_CLOSE, 0, 0); return 0; }
        break;
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) place(GetDlgItem(h, dialog_close_id), dip(h, LOWORD(l)) - 41, 6, 32, 28);
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, dialog_frame, 1);
        break;
    }
    return DefSubclassProc(h, msg, w, l);
}
struct StyledButton {
    Surface* theme;
    ButtonStyle options;
    bool native_button{}, hot{};
    std::optional<CardInk> card;
    std::function<PaperInk()> paper;
    float hover{}, pressed{}, hover_from{}, pressed_from{}, hover_to{}, pressed_to{};
    ULONGLONG hover_start{}, pressed_start{};
};
constexpr UINT_PTR animation_timer = 0x4352;
bool animated(const StyledButton& b) {
    return b.card && (b.card->part == CardPart::color_picker || b.card->part == CardPart::find_button ||
                      b.card->part == CardPart::icon_button || b.card->part == CardPart::paper_action);
}
float progress(float from, float to, ULONGLONG start, unsigned duration) {
    const float t = std::min(1.f, float(GetTickCount64() - start) / duration);
    return from + (to - from) * (1 - (1-t)*(1-t));
}
void transition(HWND h, StyledButton& b) {
    if (!animated(b)) return;
    const bool enabled = IsWindowEnabled(h) != FALSE;
    BOOL animate{};
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animate, 0);
    const float hover = enabled && b.hot ? 1.f : 0.f;
    const float press = enabled && (SendMessageW(h, BM_GETSTATE, 0, 0) & BST_PUSHED) ? 1.f : 0.f;
    if (!animate || b.theme->colors.contrast || !enabled) {
        b.hover = b.hover_from = b.hover_to = hover;
        b.pressed = b.pressed_from = b.pressed_to = press;
        KillTimer(h, animation_timer);
    } else {
        if (hover != b.hover_to) {
            b.hover_from = b.hover; b.hover_to = hover; b.hover_start = GetTickCount64();
        }
        if (press != b.pressed_to) {
            b.pressed_from = b.pressed; b.pressed_to = press; b.pressed_start = GetTickCount64();
        }
        if (b.hover != b.hover_to || b.pressed != b.pressed_to) SetTimer(h, animation_timer, 15, nullptr);
    }
}
void paint_button(HWND h, HDC dc, const StyledButton& b) {
    RECT r{};
    GetClientRect(h, &r);
    if (b.card) {
        const auto paper = b.paper ? std::optional<PaperInk>(b.paper()) : std::nullopt;
        b.theme->draw_card(h, dc, r, *b.card, b.hot, animated(b) ? b.hover : -1, animated(b) ? b.pressed : -1,
                           paper ? &*paper : nullptr);
        return;
    }
    const auto& o = b.options;
    CardInk ink{CardPart::dialog_button, o.glyph,
        o.display.empty() ? UiText(caption(h)) : o.display, {}, {}, o.color, o.selected};
    switch (o.kind) {
    case Button::primary: ink.selected = true; break;
    case Button::danger: ink.selected = true; ink.color = 1; break;
    case Button::navigation: ink.part = CardPart::settings_nav; break;
    case Button::command: ink.part = CardPart::folder_choice; break;
    case Button::swatch: ink.part = CardPart::picker_swatch; ink.label = {}; break;
    case Button::toggle: ink.part = CardPart::toggle; break;
    case Button::value: ink.part = CardPart::settings_value; break;
    default: break;
    }
    if (o.icon_only) ink.part = CardPart::icon_button;
    b.theme->draw_card(h, dc, r, ink, b.hot);
}
LRESULT CALLBACK button_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    auto& b = *reinterpret_cast<StyledButton*>(data);
    bool repaint_now = false;
    switch (m) {
    case WM_TIMER:
        if (w == animation_timer) {
            transition(h, b);
            b.hover = progress(b.hover_from, b.hover_to, b.hover_start, 100);
            b.pressed = progress(b.pressed_from, b.pressed_to, b.pressed_start, 60);
            if (b.hover == b.hover_to && b.pressed == b.pressed_to) KillTimer(h, animation_timer);
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        RECT r{};
        GetClientRect(h, &r);
        const bool inside = PtInRect(&r, {GET_X_LPARAM(l), GET_Y_LPARAM(l)}) != FALSE;
        if (inside != b.hot) {
            b.hot = inside;
            if (inside) {
                TRACKMOUSEEVENT event{sizeof(event), TME_LEAVE, h, 0};
                TrackMouseEvent(&event);
            }
            InvalidateRect(h, nullptr, FALSE);
        }
        repaint_now = b.native_button && GetCapture() == h;
        break;
    }
    case WM_MOUSELEAVE:
        b.hot = false;
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_PRINTCLIENT: {
        const auto dc = reinterpret_cast<HDC>(w);
        const int saved = SaveDC(dc);
        paint_button(h, dc, b);
        RestoreDC(dc, saved);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        paint_button(h, dc, b);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONUP:
    case WM_CANCELMODE:
    case WM_CAPTURECHANGED:
    case WM_KEYDOWN:
    case WM_KEYUP:
    case BM_SETSTATE:
        repaint_now = b.native_button;
        break;
    case WM_UPDATEUISTATE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case BM_SETCHECK:
    case WM_ENABLE:
        // Layout can set the state of a whole group. Flush once when the input
        // message completes, including any check changes made by its command.
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        KillTimer(h, animation_timer);
        RemoveWindowSubclass(h, button_proc, 1);
        delete &b;
        return DefSubclassProc(h, m, w, l);
    }
    auto result = DefSubclassProc(h, m, w, l);
    // BN_CLICKED can synchronously close the dialog and destroy this control.
    DWORD_PTR current{};
    if (GetWindowSubclass(h, button_proc, 1, &current) && current == data) {
        if (repaint_now || m == WM_MOUSEMOVE || m == WM_MOUSELEAVE || m == WM_ENABLE ||
            m == WM_SETFOCUS || m == WM_KILLFOCUS) transition(h, b);
        if (repaint_now) UpdateWindow(h);
    }
    return result;
}
LRESULT CALLBACK button_notifications(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR context) {
    if (m == language_changed) {
        if (auto* surface = reinterpret_cast<Surface*>(context)) surface->refresh();
        refresh_ui_bindings(h);
    }
    if (m == WM_NOTIFY && l) {
        const auto& header = *reinterpret_cast<const NMHDR*>(l);
        DWORD_PTR data{};
        if (header.code == NM_CUSTOMDRAW && GetWindowSubclass(header.hwndFrom, button_proc, 1, &data)) {
            const auto& b = *reinterpret_cast<const StyledButton*>(data);
            if (b.native_button) {
                const auto& draw = *reinterpret_cast<const NMCUSTOMDRAW*>(l);
                if (draw.dwDrawStage == CDDS_PREERASE) return CDRF_SKIPDEFAULT;
                if (draw.dwDrawStage == CDDS_PREPAINT) {
                    const int saved = SaveDC(draw.hdc);
                    paint_button(header.hwndFrom, draw.hdc, b);
                    RestoreDC(draw.hdc, saved);
                    return CDRF_SKIPDEFAULT;
                }
            }
        }
    }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, button_notifications, 1);
    return DefSubclassProc(h, m, w, l);
}
} // namespace
struct Surface::Graphics {
    localization::Locale locale = localization::Locale::en_us;
    std::wstring_view family;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    Microsoft::WRL::ComPtr<IDWriteFactory> write;
    Microsoft::WRL::ComPtr<IWICImagingFactory> imaging;
    Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> title, detail, glyph, card_value, card_detail,
        menu_title, menu_text, menu_glyph;
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> bucket, paint_drop, link, opacity_drop;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_stroke;
    bool icon_font{};
    std::unique_ptr<MenuBitmap> paper_pixels;
    Microsoft::WRL::ComPtr<ID2D1Bitmap> paper_bitmap;
    SIZE paper_size{};

    void draw_icon(wchar_t icon, D2D1_RECT_F area, ID2D1Brush* ink) {
        if (icon_font) {
            menu_glyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            target->DrawText(&icon, 1, menu_glyph.Get(), area, ink, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            return;
        }
        // These controls must remain recognizable even without the icon font.
        const float x = (area.left + area.right)/2, y = (area.top + area.bottom)/2;
        auto line = [&](float a, float b, float c, float d) {
            target->DrawLine({x+a,y+b}, {x+c,y+d}, ink, 1.3f);
        };
        switch (icon) {
        case Icon::chevron_up: line(-4,2,0,-2); line(0,-2,4,2); break;
        case Icon::chevron_down: line(-4,-2,0,2); line(0,2,4,-2); break;
        case Icon::check: line(-5,0,-1,4); line(-1,4,5,-4); break;
        case Icon::close: line(-4,-4,4,4); line(4,-4,-4,4); break;
        case Icon::search:
            target->DrawEllipse(D2D1::Ellipse({x-1,y-1}, 4.5f, 4.5f), ink, 1.2f);
            line(2.5f,2.5f,6,6); break;
        default:
            target->DrawRoundedRectangle(D2D1::RoundedRect({x-4,y-4,x+4,y+4},2,2),ink,1.3f);
        }
    }

    void draw_link(float cx, float cy, ID2D1Brush* ink) {
        if (!link) {
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> path;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(&path)) || FAILED(path->Open(&sink)))
                return;
            // One open link, repeated half a turn around the interlocking center.
            sink->BeginFigure({-2, 1}, D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddBezier({{0, 3.5f}, {3, 3.8f}, {5.2f, 1.6f}});
            sink->AddLine({9, -2.2f});
            sink->AddBezier({{14, -7.4f}, {7.4f, -14}, {2.2f, -9}});
            sink->AddLine({-.5f, -6.3f});
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (FAILED(sink->Close()))
                return;
            link = std::move(path);
        }
        D2D1_MATRIX_3X2_F old;
        target->GetTransform(&old);
        auto origin = D2D1::Matrix3x2F::Translation(cx, cy) * old;
        for (float sign : {1.f, -1.f}) {
            target->SetTransform(D2D1::Matrix3x2F::Scale(sign, sign) * origin);
            target->DrawGeometry(link.Get(), ink, 2.f);
        }
        target->SetTransform(old);
    }

    void draw_paint_bucket(float cx, float cy, ID2D1Brush* ink) {
        if (!bucket) {
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> body, drop;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(&body)) || FAILED(body->Open(&sink)))
                return;
            // The handle shares the upper-left edge of the can. Keeping that edge in one
            // figure avoids a double-width seam at small sizes.
            auto p = [](float x, float y) { return D2D1::Point2F((x - 67) * .305f, (y - 51.5f) * .305f); };
            sink->BeginFigure(p(61, 26), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(p(86.5f, 50.7f));
            sink->AddQuadraticBezier({p(89, 53.2f), p(87.5f, 55.8f)});
            sink->AddLine(p(84.8f, 54.6f)); // Pouring lip
            sink->AddLine(p(60.1f, 77.8f));
            sink->AddQuadraticBezier({p(56.3f, 81.6f), p(52.9f, 78.1f)});
            sink->AddLine(p(34.5f, 60.7f));
            sink->AddQuadraticBezier({p(30.6f, 57.2f), p(34.4f, 53.4f)});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->BeginFigure(p(47.4f, 40.1f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddLine(p(41.7f, 34.2f));
            sink->AddQuadraticBezier({p(39, 31.6f), p(41.8f, 28.9f)});
            sink->AddLine(p(51.4f, 20.4f));
            sink->AddQuadraticBezier({p(54.1f, 17.5f), p(56.8f, 20.4f)});
            sink->AddLine(p(62, 26.8f));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            if (FAILED(sink->Close()))
                return;
            sink.Reset();
            if (FAILED(factory->CreatePathGeometry(&drop)) || FAILED(drop->Open(&sink)))
                return;
            sink->BeginFigure(p(96.1f, 64.8f), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddBezier({p(94.1f, 68.3f), p(90.3f, 74.1f), p(90.3f, 77.8f)});
            sink->AddBezier({p(90.3f, 85.9f), p(102.4f, 85.9f), p(102.4f, 77.8f)});
            sink->AddBezier({p(102.4f, 74.4f), p(98.1f, 68.1f), p(96.1f, 64.8f)});
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close()))
                return;
            auto stroke = D2D1::StrokeStyleProperties();
            stroke.lineJoin = D2D1_LINE_JOIN_ROUND;
            stroke.startCap = stroke.endCap = D2D1_CAP_STYLE_ROUND;
            if (FAILED(factory->CreateStrokeStyle(stroke, nullptr, 0, &round_stroke)))
                return;
            bucket = std::move(body);
            paint_drop = std::move(drop);
        }
        D2D1_MATRIX_3X2_F previous;
        target->GetTransform(&previous);
        target->SetTransform(D2D1::Matrix3x2F::Translation(cx, cy) * previous);
        target->DrawGeometry(bucket.Get(), ink, 1.65f, round_stroke.Get());
        target->DrawGeometry(paint_drop.Get(), ink, 1.30f, round_stroke.Get());
        target->SetTransform(previous);
    }

    bool initialize() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf())) ||
            FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(write.GetAddressOf()))))
            return false;
        return formats();
    }
    bool formats() {
        const auto selected = ui_localization().effective_locale();
        Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts;
        UINT32 family_index{};
        BOOL found{};
        icon_font = SUCCEEDED(write->GetSystemFontCollection(&fonts)) &&
                    SUCCEEDED(fonts->FindFamilyName(L"Segoe Fluent Icons", &family_index, &found)) && found;
#ifdef CLIPROPE_DESKTOP_PROBES
        wchar_t missing[2]{};
        if (GetEnvironmentVariableW(L"CLIPROPE_PROBE_ICON_FALLBACK", missing, 2)) icon_font = false;
#endif
        Microsoft::WRL::ComPtr<IDWriteFactory2> advanced_factory;
        Microsoft::WRL::ComPtr<IDWriteFontFallback> fallback;
        if (SUCCEEDED(write.As(&advanced_factory))) advanced_factory->GetSystemFontFallback(&fallback);
        auto format = [&](const wchar_t* face, int size, DWRITE_FONT_WEIGHT weight, auto& dest) {
            Microsoft::WRL::ComPtr<IDWriteTextFormat> replacement;
            if (FAILED(write->CreateTextFormat(face, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, float(size), localization::locale_name(selected).data(),
                                               replacement.GetAddressOf())))
                return false;
            Microsoft::WRL::ComPtr<IDWriteTextFormat1> advanced;
            if (fallback && SUCCEEDED(replacement.As(&advanced))) advanced->SetFontFallback(fallback.Get());
            replacement->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            replacement->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(write->CreateEllipsisTrimmingSign(replacement.Get(), &ellipsis))) {
                DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER};
                replacement->SetTrimming(&trim, ellipsis.Get());
            }
            dest = std::move(replacement);
            return true;
        };
        const auto* face = ui_font_family();
        const bool ready = format(face, 17, DWRITE_FONT_WEIGHT_SEMI_BOLD, title) &&
               format(face, 12, DWRITE_FONT_WEIGHT_NORMAL, detail) &&
               format(L"Segoe Fluent Icons", 19, DWRITE_FONT_WEIGHT_NORMAL, glyph) &&
               format(face, 14, DWRITE_FONT_WEIGHT_NORMAL, card_value) &&
               format(face, 13, DWRITE_FONT_WEIGHT_NORMAL, card_detail) &&
               format(face, 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, menu_title) &&
               format(face, 12, DWRITE_FONT_WEIGHT_NORMAL, menu_text) &&
               format(L"Segoe Fluent Icons", 14, DWRITE_FONT_WEIGHT_NORMAL, menu_glyph);
        if (ready) { locale = selected; family = face; }
        return ready;
    }
};
thread_local std::unique_ptr<Surface::Graphics> Surface::graphics_;
namespace { thread_local unsigned targets_created{}; }
unsigned Surface::target_creations() { return targets_created; }
void Surface::discard_target() {
    if (graphics_) {
        graphics_->paper_bitmap.Reset();
        graphics_->brush.Reset();
        graphics_->target.Reset();
    }
}
bool Surface::bind(HDC dc, const RECT& area) {
    if (!graphics_) {
        auto graphics = std::make_unique<Graphics>();
        if (!graphics->initialize()) return false;
        graphics_ = std::move(graphics);
    }
    auto& g = *graphics_;
    if (!g.target) {
        const auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        if (FAILED(g.factory->CreateDCRenderTarget(&props, &g.target)) ||
            FAILED(g.target->CreateSolidColorBrush(D2D1::ColorF(0u), &g.brush))) {
            discard_target();
            return false;
        }
        ++targets_created;
    }
    if (SUCCEEDED(g.target->BindDC(dc, &area))) return true;
    discard_target();
    return false;
}
bool Surface::rounded(HDC dc, RECT area, int radius, COLORREF fill, COLORREF border, COLORREF backdrop) {
    if (IsRectEmpty(&area) || !bind(dc, area)) return false;
    auto& g = *graphics_;
    auto color = [](COLORREF c) { return D2D1::ColorF(GetRValue(c)/255.f, GetGValue(c)/255.f, GetBValue(c)/255.f); };
    g.target->SetDpi(96,96);
    g.target->BeginDraw();
    g.target->Clear(color(backdrop));
    const auto shape = D2D1::RoundedRect(D2D1::RectF(.5f,.5f,float(area.right-area.left)-.5f,
        float(area.bottom-area.top)-.5f), float(radius), float(radius));
    g.brush->SetColor(color(fill));
    g.target->FillRoundedRectangle(shape,g.brush.Get());
    g.brush->SetColor(color(border));
    g.target->DrawRoundedRectangle(shape,g.brush.Get());
    if (SUCCEEDED(g.target->EndDraw())) return true;
    discard_target();
    return false;
}
Surface::Surface() = default;
HWND Surface::dialog_chrome(HWND window) {
    attach(window);
    HWND close = child(window, L"BUTTON", tr(TextKey::common_close), WS_TABSTOP | BS_PUSHBUTTON, dialog_close_id);
    if (!close || !SetWindowSubclass(window, dialog_frame, 1, 0)) return nullptr;
    card_style(close, {CardPart::dialog_close});
    SetWindowPos(window, nullptr, 0, 0, 0, 0,
                 SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    SendMessageW(window, WM_CHANGEUISTATE, MAKEWPARAM(UIS_SET, UISF_HIDEFOCUS | UISF_HIDEACCEL), 0);
    return close;
}
void Surface::menu_header(HDC dc, RECT area, std::wstring_view title, std::wstring_view detail, wchar_t glyph) {
    const auto dpi = design_dpi(GetDpiForWindow(window_));
    if (bind(dc, area)) {
        auto& g = *graphics_;
        auto* t = g.target.Get();
        t->SetDpi(float(dpi), float(dpi));
        t->BeginDraw();
        auto color = [](COLORREF c) {
            return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f);
        };
        t->Clear(color(colors.panel));
        g.brush->SetColor(color(colors.subtle));
        t->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(8, 12, 48, 52), 9, 9), g.brush.Get());
        auto draw = [&](std::wstring_view text, IDWriteTextFormat* fmt, D2D1_RECT_F rect, COLORREF ink) {
            g.brush->SetColor(color(ink));
            t->DrawText(text.data(), UINT32(text.size()), fmt, rect, g.brush.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        };
        float width = (area.right - area.left) * 96.f / dpi;
        g.glyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        draw({&glyph, 1}, g.glyph.Get(), D2D1::RectF(8, 12, 48, 52), colors.accent);
        draw(title, g.title.Get(), D2D1::RectF(60, 9, width - 12, 35), colors.text);
        draw(detail, g.detail.Get(), D2D1::RectF(60, 33, width - 12, 54), colors.muted);
        if (SUCCEEDED(t->EndDraw()))
            return;
        discard_target();
    }
    auto line = area;
    line.bottom = line.top + px(window_, 32);
    line.left += px(window_, 12);
    draw_text(dc, line, title, Font::section, colors.text, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    line.top = line.bottom;
    line.bottom = area.bottom;
    draw_text(dc, line, detail, Font::detail, colors.muted, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
}

void Surface::card_style(HWND h, CardInk ink) {
    DWORD_PTR data{};
    if (!GetWindowSubclass(h, button_proc, 1, &data)) {
        style(h);
        if (!GetWindowSubclass(h, button_proc, 1, &data))
            return;
    }
    auto &current = reinterpret_cast<StyledButton *>(data)->card;
    if (current && *current == ink)
        return;
    current = std::move(ink);
    InvalidateRect(h, nullptr, FALSE);
}
void Surface::paper_style(HWND h, Icon icon, std::function<PaperInk()> paper) {
    card_style(h,{CardPart::paper_action,icon});
    DWORD_PTR data{};
    if (GetWindowSubclass(h,button_proc,1,&data)) reinterpret_cast<StyledButton*>(data)->paper=std::move(paper);
}
SIZE Surface::measure_paragraph(std::wstring_view text, int max_width) {
    if (!graphics_) {
        auto graphics=std::make_unique<Graphics>();
        if (graphics->initialize()) graphics_=std::move(graphics);
    }
    if (graphics_) {
        const auto dpi=design_dpi(GetDpiForWindow(window_));
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(graphics_->write->CreateTextLayout(text.data(),UINT32(text.size()),graphics_->detail.Get(),
                    std::max(1.f,max_width*96.f/dpi),100000.f,&layout))) {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            DWRITE_TRIMMING trim{}; layout->SetTrimming(&trim,nullptr);
            DWRITE_TEXT_METRICS metrics{};
            if (SUCCEEDED(layout->GetMetrics(&metrics)))
                return {std::min<LONG>(max_width,LONG(std::ceil(metrics.widthIncludingTrailingWhitespace*dpi/96))),
                        LONG(std::ceil(metrics.height*dpi/96))};
        }
    }
    auto dc=GetDC(window_); auto old=SelectObject(dc,get(Font::detail));
    RECT bounds{0,0,max_width,0};
    DrawTextW(dc,text.data(),int(text.size()),&bounds,DT_CALCRECT|DT_WORDBREAK|DT_NOPREFIX);
    SelectObject(dc,old); ReleaseDC(window_,dc);
    return {bounds.right,bounds.bottom};
}
void Surface::draw_paragraph(HDC dc, RECT bounds, std::wstring_view text, COLORREF ink, COLORREF background) {
    if (bind(dc,bounds)) {
        auto& g=*graphics_; const auto dpi=design_dpi(GetDpiForWindow(window_));
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(g.write->CreateTextLayout(text.data(),UINT32(text.size()),g.detail.Get(),
                (bounds.right-bounds.left)*96.f/dpi,(bounds.bottom-bounds.top)*96.f/dpi,&layout))) {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
            DWRITE_TRIMMING trim{}; layout->SetTrimming(&trim,nullptr);
            auto color=[](COLORREF c) { return D2D1::ColorF(GetRValue(c)/255.f,GetGValue(c)/255.f,GetBValue(c)/255.f); };
            g.target->SetDpi(float(dpi),float(dpi)); g.target->BeginDraw();
            g.target->Clear(color(background)); g.brush->SetColor(color(ink));
            g.target->DrawTextLayout({0,0},layout.Get(),g.brush.Get(),D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (SUCCEEDED(g.target->EndDraw())) return;
            discard_target();
        }
    }
    solid(dc,bounds,background);
    draw_text(dc,bounds,text,Font::detail,ink,DT_WORDBREAK|DT_NOPREFIX);
}
namespace {
// Only rest-state control artwork enters this cache. The data contains no HWNDs,
// callbacks, card content or rope names. Hover, focus and pressed frames draw live.
struct TileKey {
    CardInk ink;
    Palette palette;
    int width, height;
    UINT dpi;
    bool enabled;
    std::uint64_t language_revision;
    localization::Locale locale;
    bool operator==(const TileKey&) const = default;
};
struct Tile {
    TileKey key;
    MenuBitmap bitmap;
    size_t bytes;
    Tile(TileKey k, size_t n) : key(std::move(k)), bytes(n) {}
};
thread_local std::list<Tile> tiles;
thread_local size_t tile_bytes{};
}
size_t Surface::control_artwork_bytes() { return tile_bytes; }
void Surface::clear_control_artwork() {
    tiles.clear();
    tile_bytes = 0;
}
void Surface::draw_card(HWND h, HDC dc, RECT area, const CardInk& ink, bool hot, float hover, float press, const PaperInk* paper) {
    const int width = area.right - area.left, height = area.bottom - area.top;
    const bool child = (GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD) != 0;
    if (width <= 0 || height <= 0 || !RectVisible(dc, &area)) return;
    // Dialogs and tray previews can contain profile-specific text.
    // Cache only reusable menu artwork and the text-free application identity.
    if ((ink.part >= CardPart::dialog_close && ink.part != CardPart::brand_icon) || hover >= 0 || press >= 0) {
        paint_card(h, dc, area, ink, hot, hover, press, paper);
        return;
    }
    const bool resting = !child || (!hot && GetFocus() != h && !(SendMessageW(h, BM_GETSTATE, 0, 0) & BST_PUSHED));
    if (resting && ink.part != CardPart::rope_header && ink.part != CardPart::slider && ink.part != CardPart::toggle) {
        TileKey key{ink, colors, width, height, design_dpi(GetDpiForWindow(window_)), IsWindowEnabled(h) != FALSE, ui_localization().revision(), ui_localization().effective_locale()};
        auto hit = std::find_if(tiles.begin(), tiles.end(), [&](const auto& tile) { return tile.key == key; });
        const size_t bytes = size_t(width) * height * 4;
        if (hit == tiles.end() && bytes <= artwork_budget) {
            while (!tiles.empty() && (tile_bytes + bytes > artwork_budget || tiles.size() >= 64)) {
                tile_bytes -= tiles.back().bytes;
                tiles.pop_back();
            }
            auto& tile = tiles.emplace_front(std::move(key), bytes);
            if (tile.bitmap.create(width, height) && paint_card(h, tile.bitmap.dc, {0, 0, width, height}, ink, false)) {
                tile_bytes += bytes;
                hit = tiles.begin();
            } else tiles.pop_front();
        }
        if (hit != tiles.end()) {
            tiles.splice(tiles.begin(), tiles, hit);
            if (BitBlt(dc, area.left, area.top, width, height, hit->bitmap.dc, 0, 0, SRCCOPY)) return;
        }
    }
    paint_card(h, dc, area, ink, hot);
}
bool Surface::paint_card(HWND h, HDC dc, RECT area, const CardInk& ink, bool hot, float hover, float press, const PaperInk* paper) {
    const auto dpi = design_dpi(GetDpiForWindow(window_));
    if (!bind(dc, area)) {
        solid(dc, area, ink.part == CardPart::brand_icon ? colors.subtle : colors.panel);
        const bool menu = ink.part < CardPart::dialog_close;
        const bool header = ink.part == CardPart::header || ink.part == CardPart::rope_header;
        const UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
        RECT label = area;
        if (ink.part == CardPart::tray_quick_note) {
            label.left += px(window_, 31);
            RECT shortcut = label;
            shortcut.right -= px(window_, 5);
            draw_text(dc, shortcut, ink.hint, Font::menu, colors.muted, flags | DT_RIGHT);
        } else if (menu && !ink.detail.empty()) {
            RECT detail = area;
            if (ink.part == CardPart::rope_style) {
                label.right = area.left + px(window_, 90);
                detail.left = label.right;
            } else {
                label.bottom = detail.top = (area.top + area.bottom) / 2;
            }
            draw_text(dc, detail, ink.detail, Font::menu, colors.muted, flags);
        }
        draw_text(dc, label, ink.label, header ? Font::menu_title : menu ? Font::menu : Font::body, colors.text, flags);
        return false;
    }
    auto& g = *graphics_;
    auto* t = g.target.Get();
    auto color = [](COLORREF c) {
        return D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f);
    };
    auto blend = [](COLORREF a, COLORREF b, float k) {
        return RGB(int(GetRValue(a) * (1 - k) + GetRValue(b) * k), int(GetGValue(a) * (1 - k) + GetGValue(b) * k),
                   int(GetBValue(a) * (1 - k) + GetBValue(b) * k));
    };
    const float w = (area.right - area.left) * 96.f / dpi, height = (area.bottom - area.top) * 96.f / dpi,
                cy = height / 2;
    auto& p = colors;
    const COLORREF text = p.contrast ? p.text : blend(p.text, p.accent, .08f);
    const COLORREF active = p.contrast ? p.accent : blend(p.accent, RGB(208, 123, 50), p.dark ? .12f : .30f);
    const COLORREF soft = p.contrast ? p.panel : blend(p.panel, active, p.dark ? .13f : .12f);
    const COLORREF band = p.contrast ? p.panel : blend(p.panel, active, p.dark ? .14f : .085f);
    const auto background = ink.on_band ? band : p.panel;
    const bool enabled = IsWindowEnabled(h) != FALSE;
    const bool down = (GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD) && (SendMessageW(h, BM_GETSTATE, 0, 0) & BST_PUSHED);
    const float hover_amount = enabled ? (hover < 0 ? float(hot) : hover) : 0.f;
    const float pushed = enabled ? (press < 0 ? float(down) : press) : 0.f;
    t->SetDpi(float(dpi), float(dpi));
    t->BeginDraw();
    t->Clear(color(paper ? paper->background : background));
    if (paper) {
        const int width=area.right-area.left, pixels_high=area.bottom-area.top;
        if (!g.paper_pixels || g.paper_size.cx!=width || g.paper_size.cy!=pixels_high) {
            g.paper_pixels=std::make_unique<MenuBitmap>();
            if (!g.paper_pixels->create(width,pixels_high)) g.paper_pixels.reset();
            g.paper_size={width,pixels_high}; g.paper_bitmap.Reset();
        }
        if (g.paper_pixels) {
            POINT origin{}; MapWindowPoints(h,GetParent(h),&origin,1);
            SetBrushOrgEx(g.paper_pixels->dc,-origin.x,-origin.y,nullptr);
            RECT bounds{0,0,width,pixels_high};
            if (paper->texture) FillRect(g.paper_pixels->dc,&bounds,paper->texture);
            else solid(g.paper_pixels->dc,bounds,paper->background);
            GdiFlush();
            if (!g.paper_bitmap) g.target->CreateBitmap(D2D1::SizeU(width,pixels_high),
                D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,D2D1_ALPHA_MODE_IGNORE)),
                &g.paper_bitmap);
            if (g.paper_bitmap && SUCCEEDED(g.paper_bitmap->CopyFromMemory(nullptr,g.paper_pixels->pixels,width*4)))
                t->DrawBitmap(g.paper_bitmap.Get(),D2D1::RectF(0,0,w,height));
        }
    }
    auto brush = [&](COLORREF c) {
        g.brush->SetColor(color(c));
        return g.brush.Get();
    };
    auto box = [&](float l, float top, float r, float b, float radius, COLORREF fill, COLORREF edge,
                   float stroke = 0.f) {
        auto shape = D2D1::RoundedRect(D2D1::RectF(l, top, r, b), radius, radius);
        t->FillRoundedRectangle(shape, brush(fill));
        if (stroke)
            t->DrawRoundedRectangle(shape, brush(edge), stroke);
    };
    auto circle = [&](float x, float y, float rad, COLORREF fill) {
        t->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), rad, rad), brush(fill));
    };
    auto line = [&](float x, float y, float ex, float ey, COLORREF c, float weight = 1.f) {
        t->DrawLine(D2D1::Point2F(x, y), D2D1::Point2F(ex, ey), brush(c), weight);
    };
    auto draw = [&](std::wstring_view value, IDWriteTextFormat* fmt, float l, float top, float r, float b,
                    COLORREF c, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        fmt->SetTextAlignment(align);
        t->DrawText(value.data(), UINT32(value.size()), fmt, D2D1::RectF(l, top, r, b), brush(c),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    const auto center = DWRITE_TEXT_ALIGNMENT_CENTER, right = DWRITE_TEXT_ALIGNMENT_TRAILING;
    const bool focus = GetFocus() == h && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> brand;
    switch (ink.part) {
    case CardPart::paper_action: {
        const bool highlight = hover_amount > 0 || pushed > 0;
        const auto foreground = p.contrast ? (highlight ? p.on_accent : p.text) : paper ? paper->foreground : p.text;
        if (p.contrast) box(1,1,w-1,height-1,4,highlight ? p.accent : p.panel,p.line,focus ? 1.f : 0.f);
        if (hover_amount > 0 || pushed > 0) {
            auto* overlay = brush(foreground);
            overlay->SetOpacity(p.contrast ? 0.f : .08f*hover_amount + .10f*pushed);
            t->FillRoundedRectangle(D2D1::RoundedRect({1,1,w-1,height-1},4,4),overlay);
            overlay->SetOpacity(1);
        }
        g.draw_icon(ink.glyph,D2D1::RectF(0,0,w,height),brush(enabled ? foreground : p.muted));
        break;
    }
    case CardPart::list_item: {
        const bool selected = enabled && ink.selected;
        const auto fill = selected ? (p.contrast ? p.accent : soft) : hot && enabled ? p.subtle : p.panel;
        box(1, 2, w-1, height-2, 6, fill, p.line, 0);
        if (selected && focus)
            box(1.5f, 2.5f, w-1.5f, height-2.5f, 6, fill, active, 1.f);
        draw(ink.label, g.card_value.Get(), 8, 0, w-8, height,
             !enabled ? p.muted : selected && p.contrast ? p.on_accent : p.text);
        break;
    }
    case CardPart::tray_header:
    case CardPart::tray_band:
    case CardPart::tray_preview:
    case CardPart::tray_quick_note:
    case CardPart::tray_settings:
    case CardPart::tray_exit: {
        const bool highlight = enabled && (hot || down);
        const auto sheet = p.contrast ? p.panel : p.dark ? band : RGB(249, 245, 242);
        const auto header = p.contrast ? p.panel : p.dark ? soft : RGB(245, 240, 236);
        const auto over = p.contrast ? p.accent : blend(header, active, down ? .22f : .12f);
        const bool selected_contrast = highlight && p.contrast;
        const auto label = selected_contrast ? p.on_accent : text;
        const auto muted = selected_contrast ? p.on_accent : p.muted;
        const auto symbol = std::wstring_view(&ink.glyph, 1);
        auto chevron = [&](float x, float y) {
            line(x, y - 4, x + 4, y, muted, 1.4f);
            line(x + 4, y, x, y + 4, muted, 1.4f);
        };
        if (ink.part == CardPart::tray_header || ink.part == CardPart::tray_band) {
            const bool head = ink.part == CardPart::tray_header;
            box(.5f, .5f, w - .5f, height - .5f, 7, head ? (highlight ? over : header) : sheet,
                p.text, p.contrast ? 1.f : 0.f);
            if (head) {
                box(5, cy - 12, 29, cy + 12, 6, p.contrast ? p.panel : blend(header, active, .09f), p.text);
                draw(symbol, g.menu_glyph.Get(), 5, 0, 29, height, p.contrast ? p.text : active, center);
                draw(ink.detail, g.detail.Get(), 35, 0, 105, height, muted);
                draw(ink.label, g.menu_title.Get(), 108, 0, w - 18, height, label);
                chevron(w - 12, cy);
            }
        } else if (ink.part == CardPart::tray_preview) {
            t->Clear(color(sheet));
            if (highlight) box(0, 0, w, height, 5, over, over);
            if (ink.glyph) {
                draw(symbol, g.menu_glyph.Get(), 2, 0, 20, height, muted, center);
                draw(ink.hint, g.menu_text.Get(), 24, 0, 39, height, label);
            }
            draw(ink.label, g.menu_text.Get(), ink.glyph ? 39.f : 6.f, 0, w - 4, height, enabled ? label : muted);
        } else if (ink.part == CardPart::tray_quick_note) {
            if (highlight) box(0, 1, w, height - 1, 6, over, over);
            // An open paper outline leaves room for the pencil at its upper right.
            auto stroke = [&](float x1, float y1, float x2, float y2) {
                line(7 + x1, cy - 7 + y1, 7 + x2, cy - 7 + y2, muted, 1.f);
            };
            stroke(6.5f, 2.5f, 1.5f, 2.5f);
            stroke(1.5f, 2, 1.5f, 12.5f);
            stroke(1, 12.5f, 11.5f, 12.5f);
            stroke(11.5f, 13, 11.5f, 8);
            stroke(5, 9.5f, 5.7f, 7);
            stroke(5.7f, 7, 11.6f, 1.1f);
            stroke(11.3f, 1.4f, 13.4f, 3.5f);
            stroke(13.4f, 3, 7.5f, 8.9f);
            stroke(7.5f, 8.9f, 5, 9.5f);
            stroke(10.1f, 2.6f, 12, 4.5f);
            draw(ink.label, g.menu_text.Get(), 31, 0, 95, height, label);
            draw(ink.hint, g.detail.Get(), 97, 0, w - 5, height, muted, right);
        } else {
            const bool settings = ink.part == CardPart::tray_settings;
            const auto danger = selected_contrast ? p.on_accent : p.danger;
            if (highlight) box(0, 1, w, height - 1, 6, over, over);
            draw(symbol, g.menu_glyph.Get(), 4, 0, 24, height, settings ? muted : danger, center);
            draw(ink.label, g.menu_text.Get(), 31, 0, w - 18, height,
                 settings ? label : danger);
            if (settings) {
                chevron(w - 12, cy);
            }
        }
        break;
    }
    case CardPart::folder_choice:
        if (hot || down) box(0, 0, w, height, 7, soft, p.line);
        draw({&ink.glyph, 1}, g.glyph.Get(), 6, 0, 34, height, p.muted, center);
        draw(ink.label, g.card_value.Get(), 43, 0, w-6, height, text);
        break;
    case CardPart::find_button:
    case CardPart::icon_button:
    case CardPart::dialog_close: {
        if (ink.part == CardPart::dialog_close) t->Clear(color(ink.on_panel ? p.panel : p.window));
        const auto base = ink.part == CardPart::dialog_close && !ink.on_panel ? p.window : p.panel;
        if (hover_amount > 0 || pushed > 0) box(1, 1, w-1, height-1, 6,
            blend(base, p.text, .07f*hover_amount + .09f*pushed), p.line);
        g.draw_icon(ink.part == CardPart::dialog_close ? Icon::close : ink.glyph,
                    D2D1::RectF(0, 0, w, height), brush(enabled ? p.text : p.muted));
        break;
    }
    case CardPart::dialog_button: {
        t->Clear(color(ink.on_panel ? p.panel : p.window));
        auto fill = !enabled ? p.subtle : ink.selected ? (ink.color ? p.danger : p.accent) : p.panel;
        if (enabled && (hot || down)) fill = blend(fill, p.text, .08f);
        box(.7f, .7f, w-.7f, height-.7f, 7, fill, ink.selected ? fill : p.line, 1);
        draw(ink.label, g.card_value.Get(), 4, 0, w-4, height, !enabled ? p.muted : ink.selected ? p.on_accent : p.text, center);
        break;
    }
    case CardPart::dialog_field:
        t->Clear(color(p.window));
        box(.6f, .6f, w-.6f, height-.6f, 6, p.panel, ink.selected ? p.accent : p.line, 1);
        break;
    case CardPart::picker_preview:
        box(.5f, .5f, w-.5f, height-.5f, std::min(6.f, height/4), ink.color,
            hot ? p.accent : blend(ink.color, p.muted, .18f), .6f);
        break;
    case CardPart::picker_action: {
        auto fill = ink.selected ? p.accent : inset_surface(p);
        if (!enabled) fill = p.subtle;
        else if (hot || down) fill = blend(fill, p.text, .08f);
        box(.5f, 2, w-.5f, height-2, 6, fill, ink.selected ? fill : p.line, .8f);
        draw(ink.label, g.menu_text.Get(), 4, 0, w-4, height,
             !enabled ? p.muted : ink.selected ? p.on_accent : p.text, center);
        break;
    }
    case CardPart::picker_field:
        box(.5f, .5f, w-.5f, height-.5f, 6, inset_surface(p),
            ink.selected ? p.accent : p.line, .8f);
        break;
    case CardPart::panel:
        t->Clear(color(p.window));
        box(.6f, .6f, w-.6f, height-.6f, 11, p.panel, p.line, 1);
        break;
    case CardPart::brand_icon: {
        t->Clear(color(p.subtle));
        if (!g.imaging)
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g.imaging));
        Microsoft::WRL::ComPtr<IWICBitmap> source;
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        const auto pixels = std::max(1, int(std::lround(std::min(w, height) * dpi / 96.f)));
        if (g.imaging && SUCCEEDED(g.imaging->CreateBitmapFromHICON(application_icon(pixels), &source)) &&
            SUCCEEDED(g.imaging->CreateFormatConverter(&converter)) &&
            SUCCEEDED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                           WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
            SUCCEEDED(t->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &brand)))
            t->DrawBitmap(brand.Get(), D2D1::RectF(0, 0, w, height), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        break;
    }
    case CardPart::settings_nav:
        t->Clear(color(p.subtle));
        if (ink.selected || hot)
            box(0, 0, w, height, 8, ink.selected ? p.panel : blend(p.panel,p.subtle,.55f), p.line);
        if (ink.selected) box(0, 6, 3.5f, height-6, 1.75f, p.accent, p.accent);
        draw({&ink.glyph,1}, g.glyph.Get(), ink.label.empty() ? 0.f : 7.f, 0,
             ink.label.empty() ? w : 29.f, height, ink.selected ? p.accent : p.muted, center);
        draw(ink.label, g.card_value.Get(), 38, 0, w-4, height, ink.selected ? p.accent : p.text);
        break;
    case CardPart::settings_choice:
        t->Clear(color(p.window));
        box(.6f, .6f, w-.6f, height-.6f, 5, !enabled ? p.subtle : hot ? soft : p.panel, focus ? active : p.line, 1);
        draw(ink.label, g.card_value.Get(), 9, 0, w-29, height, enabled ? text : p.muted);
        g.draw_icon(Icon::chevron_down, D2D1::RectF(w-26, 0, w-4, height), brush(enabled ? p.text : p.muted));
        break;
    case CardPart::settings_switch: {
        t->Clear(color(ink.on_panel ? p.panel : p.window));
        const bool on = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
        box(w-40, cy-10, w-1, cy+10, 10, on && enabled ? p.accent : p.subtle, on && enabled ? p.accent : p.muted, 1);
        circle(on ? w-11.f : w-30.f, cy, 7.5f, on && enabled ? p.on_accent : p.muted);
        break;
    }
    case CardPart::settings_value:
        if (ink.selected) box(.6f, 1, w-.6f, height-1, 6, p.subtle, p.line, 1);
        draw(ink.label, g.card_detail.Get(), 3, 0, w-(ink.selected ? 3 : 1), height, p.muted, ink.selected ? center : right);
        break;
    case CardPart::rope_header: {
        g.draw_link(15, cy, brush(active));
        draw(ink.label, g.menu_title.Get(), 32, cy - 18, w - 3, cy + 1, text);
        const UiText name = tr(TextKey::tray_current_rope) + L": " + ink.detail;
        const UiText suffix = L"  ·  " + tr(TextKey::common_auto_save);
        auto measure = [&](std::wstring_view value) {
            Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
            DWRITE_TEXT_METRICS m{};
            g.menu_text->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            if (SUCCEEDED(g.write->CreateTextLayout(value.data(), UINT32(value.size()), g.menu_text.Get(),
                                                     65535.f, 28.f, &layout)))
                layout->GetMetrics(&m);
            return std::ceil(m.widthIncludingTrailingWhitespace);
        };
        const float start = 32, end = w - 3;
        const float split = std::clamp(start + measure(name), start, std::max(start, end - measure(suffix) - 1));
        draw(name, g.menu_text.Get(), start, cy + 1, split, cy + 18, p.muted);
        draw(suffix, g.menu_text.Get(), split, cy + 1, end, cy + 18, p.muted);
        break;
    }
    case CardPart::band:
        box(0, 0, w, height, 9, band, p.line, p.contrast ? 1.f : 0.f);
        break;
    case CardPart::input:
        box(.6f, 1, w - .6f, height - 1, 6, p.panel, ink.selected ? active : p.line, 1.f);
        break;
    case CardPart::color_picker:
        if (!ink.on_panel) t->Clear(color(p.window));
        box(.6f, 1, w - .6f, height - 1, 6,
            !enabled ? p.subtle : blend(p.panel, p.text, .06f*hover_amount + .08f*pushed), p.line, 1.f);
        box(w < 48 ? (w-24)/2 : 8, cy - 12, w < 48 ? (w+24)/2 : 32, cy + 12, 5, ink.color, p.line, 1.f);
        if(w >= 48) {
            draw(ink.label, g.menu_text.Get(), 39, 0, w - 22, height, enabled ? text : p.muted, center);
            g.draw_icon(Icon::chevron_down, D2D1::RectF(w-22, 0, w-4, height), brush(p.muted));
        }
        break;
    case CardPart::segment_left:
    case CardPart::segment_middle:
    case CardPart::segment_right: {
        if (!ink.on_panel && !ink.on_band) t->Clear(color(p.window));
        const bool left = ink.part == CardPart::segment_left;
        box(left ? .6f : -8.f, 1, ink.part != CardPart::segment_right ? w + 8.f : w - .6f, height - 1, 7, p.panel, p.line, 1.f);
        if (ink.selected || hot)
            box(2, 3, w - 2, height - 3, 6, ink.selected ? active : soft, active);
        draw(ink.label, g.menu_text.Get(), 2, 0, w - 2, height, ink.selected ? p.on_accent : text, center);
        break;
    }
    case CardPart::header: {
        // Dedicated settings bag, with the handle behind the face.
        auto handle = D2D1::RoundedRect(D2D1::RectF(14, cy - 11, 22, cy - 4), 3, 3);
        t->DrawRoundedRectangle(handle, brush(active), 1.8f);
        box(9, cy - 7, 27, cy + 12, 2.4f, p.panel, active, 1.8f);
        const float gear_y = cy + 2.5f;
        circle(18, gear_y, 4.2f, active);
        for (int i = 0; i < 8; ++i) {
            const float angle = i * std::numbers::pi_v<float> / 4;
            const float x = std::cos(angle), y = std::sin(angle);
            line(18 + 3.5f * x, gear_y + 3.5f * y, 18 + 5.4f * x, gear_y + 5.4f * y, active, 2.1f);
        }
        circle(18, gear_y, 1.75f, p.panel);
        draw(ink.label, g.menu_title.Get(), 32, cy - 18, w - 3, cy + 1, text);
        draw(ink.detail, g.menu_text.Get(), 32, cy + 1, w - 3, cy + 18, p.muted);
        break;
    }
    case CardPart::action:
    case CardPart::rope_style:
    case CardPart::rope_action:
    case CardPart::label:
    case CardPart::toggle: {
        const bool style = ink.part == CardPart::rope_style;
        const bool rope_action = style || ink.part == CardPart::rope_action;
        const bool action = ink.part == CardPart::action || rope_action;
        const bool highlight = enabled && (hot || down || (action && ink.selected));
        if (highlight)
            box(.75f, 1, w - .75f, height - 1, 9, down ? p.subtle : soft, p.accent, p.contrast ? 1.f : 0.f);
        const auto label_color = !enabled ? p.muted : rope_action && ink.glyph == Icon::erase ? p.danger
                          : rope_action && ink.glyph == Icon::clear ? active : text;
        const auto icon_color = rope_action && (ink.glyph == Icon::clear || ink.glyph == Icon::erase) ? label_color
                                : highlight || (ink.on_band && ink.glyph == Icon::overview) ? active : p.muted;
        constexpr float label_left = 32, cx = 15;
        switch (ink.glyph) {
        case Icon::density: // Two density panes
            box(cx - 8, cy - 6.5f, cx + 8, cy + 6.5f, 1.5f, background, icon_color, 1.6f);
            line(cx + 1, cy - 6.5f, cx + 1, cy + 6.5f, icon_color, 1.5f);
            break;
        case Icon::overview: // Four ropes in overview
            for (float dx : {-8.f, 1.f})
                for (float dy : {-8.f, 1.f})
                    box(cx + dx, cy + dy, cx + dx + 6.5f, cy + dy + 6.5f, 1.3f, background, icon_color, 1.5f);
            break;
        case Icon::font_size:
            draw(L"Aa", g.menu_title.Get(), 2, 0, 28, height, icon_color, center);
            break;
        case Icon::bold:
            draw(L"B", g.menu_title.Get(), 2, 0, 28, height, icon_color, center);
            break;
        case Icon::ink:
            draw(L"A", g.menu_title.Get(), 2, cy - 12, 28, cy + 8, icon_color, center);
            line(cx - 7.5f, cy + 8.1f, cx + 7.5f, cy + 8.1f, icon_color, 1.8f);
            break;
        case Icon::appearance:
            if (rope_action)
                draw({&ink.glyph, 1}, g.menu_glyph.Get(), 2, 0, 28, height, icon_color, center);
            else
                g.draw_paint_bucket(cx, cy, brush(icon_color));
            break;
        case Icon::clear: { // Broom: sloped handle, flared bristles and a shorter inner stroke.
            line(cx + 1, cy - 2, cx + 9, cy - 11, icon_color, 1.8f);
            line(cx - 3, cy - 3, cx + 4, cy + 2, icon_color, 1.7f);
            line(cx + 4, cy + 2, cx - 1, cy + 10, icon_color, 1.7f);
            line(cx - 1, cy + 10, cx - 10, cy + 4, icon_color, 1.7f);
            line(cx - 10, cy + 4, cx - 3, cy - 3, icon_color, 1.7f);
            line(cx - 6, cy + 7, cx - 3, cy + 3, icon_color, 1.3f);
            break;
        }
        case Icon::add:
            t->DrawEllipse(D2D1::Ellipse({cx, cy}, 8, 8), brush(icon_color), 1.7f);
            line(cx - 4, cy, cx + 4, cy, icon_color, 1.6f);
            line(cx, cy - 4, cx, cy + 4, icon_color, 1.6f);
            break;
        case Icon::opacity: {
            if (!g.opacity_drop) {
                Microsoft::WRL::ComPtr<ID2D1PathGeometry> shape;
                Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(g.factory->CreatePathGeometry(&shape)) && SUCCEEDED(shape->Open(&sink))) {
                    sink->BeginFigure({0, -9}, D2D1_FIGURE_BEGIN_HOLLOW);
                    sink->AddBezier({{-2, -6}, {-6.5f, -.5f}, {-6.5f, 3}});
                    sink->AddBezier({{-6.5f, 11}, {6.5f, 11}, {6.5f, 3}});
                    sink->AddBezier({{6.5f, -.5f}, {2, -6}, {0, -9}});
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    if (SUCCEEDED(sink->Close())) g.opacity_drop = std::move(shape);
                }
            }
            if (g.opacity_drop) {
                D2D1_MATRIX_3X2_F old;
                t->GetTransform(&old);
                t->SetTransform(D2D1::Matrix3x2F::Translation(cx, cy) * old);
                t->DrawGeometry(g.opacity_drop.Get(), brush(icon_color), 1.6f);
                t->SetTransform(old);
            }
            break;
        }
        default:
            if (ink.glyph)
                draw({&ink.glyph, 1}, g.menu_glyph.Get(), 2, 0, 28, height, icon_color, center);
        }
        const bool two = action && !style && !ink.detail.empty();
        const float trailing = ink.chevron ? 22.f : ink.hint.empty() ? 0.f : rope_action ? 118.f : 40.f;
        draw(ink.label, g.menu_text.Get(), label_left, 0, style ? 100.f : w - trailing,
             two ? cy : height, label_color);
        if (two)
            draw(ink.detail, g.menu_text.Get(), label_left, cy, w - trailing, height, p.muted);
        if (style)
            draw(ink.detail, g.menu_text.Get(), 100, 0, w - 4, height, p.muted, right);
        if (ink.chevron || (rope_action && !ink.hint.empty())) {
            line(w - 15, cy - 4.5f, w - 10.5f, cy, p.muted, 1.6f);
            line(w - 10.5f, cy, w - 15, cy + 4.5f, p.muted, 1.6f);
            if (!ink.chevron)
                draw(ink.hint, g.menu_text.Get(), w - trailing, 0, w - 23, height, p.muted, right);
        } else if (!ink.hint.empty())
            draw(ink.hint, g.menu_text.Get(), w - trailing, 0, w - 6, height, p.muted, right);
        if (ink.part == CardPart::toggle) {
            bool on = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
            const float end = w - 9;
            box(end - 30, cy - 8, end, cy + 8, 8, on ? active : p.subtle, p.contrast ? p.text : p.line,
                on && !p.contrast ? 0.f : .85f);
            circle(on ? end - 8 : end - 22, cy, 5.5f,
                   on ? p.on_accent : (p.contrast ? p.text : blend(p.muted, p.panel, .25f)));
        }
        break;
    }
    case CardPart::decrease:
    case CardPart::increase:
    case CardPart::size_value: {
        const bool left = ink.part == CardPart::decrease, last = ink.part == CardPart::increase;
        auto bg = left || last ? blend(p.panel, p.subtle, hot ? 1.f : .70f) : p.panel;
        box(left ? .6f : -9.f, 1, w + (last ? -.6f : 9.f), height - 1, 7, down ? soft : bg, p.line, 1.f);
        if (!last)
            line(w - .5f, 1, w - .5f, height - 1, p.line);
        if (ink.on_band && (left || last)) {
            if (last)
                line(.5f, 1, .5f, height - 1, p.line);
            line(w / 2 - 4, cy, w / 2 + 4, cy, p.muted, 1.4f);
            if (last)
                line(w / 2, cy - 4, w / 2, cy + 4, p.muted, 1.4f);
        } else
            draw(ink.label, g.menu_text.Get(), 1, 0, w - 1, height, left || last ? p.muted : text, center);
        break;
    }
    case CardPart::percent_value:
        draw(ink.label, g.menu_text.Get(), 0, 0, w, height, p.muted, right);
        break;
    case CardPart::dot:
    case CardPart::picker_swatch:
    case CardPart::tile: {
        const float cx = w / 2, r = std::min({ink.part == CardPart::picker_swatch ? 11.f : 9.7f, cx - 4, cy - 4});
        const bool dot = ink.part != CardPart::tile;
        auto shape = [&](float rad, COLORREF fill, COLORREF edge, float stroke) {
            box(cx - rad, cy - rad, cx + rad, cy + rad, dot ? rad : 4.f + (rad - r) * .6f, fill, edge, stroke);
        };
        if (ink.selected)
            shape(r + 2.9f, background, active, 1.35f);
        shape(r, ink.color, p.contrast ? p.text : blend(ink.color, p.muted, .23f), dot ? .35f : .65f);
        if (ink.part == CardPart::picker_swatch && ink.selected) {
            const auto c = ink.color;
            const auto check = (GetRValue(c)*299 + GetGValue(c)*587 + GetBValue(c)*114) > 145000
                                   ? RGB(45,39,32) : RGB(255,255,255);
            line(cx-4,cy,cx-1,cy+3,check,1.8f);
            line(cx-1,cy+3,cx+5,cy-3,check,1.8f);
        }
        if (ink.part == CardPart::picker_swatch && !ink.label.empty()) {
            const auto c = ink.color;
            const auto foreground = (GetRValue(c)*299 + GetGValue(c)*587 + GetBValue(c)*114) > 145000 ? RGB(32,28,24) : RGB(255,255,255);
            draw(ink.label, g.menu_text.Get(), 0, 0, w, height, foreground, center);
        }
        if (hot && !ink.selected) {
            auto halo = D2D1::RoundedRect(D2D1::RectF(cx - r - 2, cy - r - 2, cx + r + 2, cy + r + 2),
                                          dot ? r + 2 : 5.f, dot ? r + 2 : 5.f);
            t->DrawRoundedRectangle(halo, brush(p.line));
        }
        break;
    }
    case CardPart::slider: {
        RECT thumb{};
        SendMessageW(h, TBM_GETTHUMBRECT, 0, LPARAM(&thumb));
        const float pad = 7, x = std::clamp((thumb.left + thumb.right) * 48.f / dpi, pad, std::max(pad, w - pad));
        box(pad, cy - 2, w - pad, cy + 2, 2, p.subtle, p.line);
        if (x > pad)
            box(pad, cy - 2, x, cy + 2, 2, active, active);
        circle(x, cy + 1.2f, 7.2f, p.contrast ? p.panel : blend(p.panel, p.muted, .14f));
        circle(x, cy, 7, p.contrast ? p.text : p.line);
        circle(x, cy, 6, p.panel);
        break;
    }
    }
    if (focus && ink.part != CardPart::list_item && ink.part != CardPart::panel &&
        (GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD)) {
        auto outline = D2D1::RoundedRect(D2D1::RectF(1.1f, 1.1f, w - 1.1f, height - 1.1f), 6, 6);
        t->DrawRoundedRectangle(outline, brush(active), 1.f);
    }
    // Failed icon conversion must not retain an empty tile that prevents retry.
    if (SUCCEEDED(t->EndDraw())) return ink.part != CardPart::brand_icon || brand.Get() != nullptr;
    discard_target();
    return false;
}
Surface::~Surface() { release(); }
void Surface::release() {
    for (auto& f : fonts_) {
        if (f)
            DeleteObject(f);
        f = nullptr;
    }
    for (auto& b : brushes_) {
        if (b)
            DeleteObject(b);
        b = nullptr;
    }
    window_ = nullptr;
}
void Surface::attach(HWND h) {
    window_ = h;
    SetWindowSubclass(h, button_notifications, 1, reinterpret_cast<DWORD_PTR>(this));
    refresh();
}
void Surface::refresh() {
    if (graphics_ && (graphics_->locale != ui_localization().effective_locale() || graphics_->family != ui_font_family())) graphics_->formats();
    colors = system_palette();
    window_frame(window_, colors);
    constexpr int sizes[] = {14, 12, 17, 24, 16, 12, 14, 10, 12};
    auto old = fonts_;
    for (size_t i = 0; i < fonts_.size(); ++i) {
        const auto role = static_cast<Font>(i);
        const bool semibold = role == Font::section || role == Font::title || role == Font::menu_title || role == Font::caption_bold;
        const auto face = role == Font::icon ? L"Segoe Fluent Icons" : ui_font_family();
        fonts_[i] =
            CreateFontW(-px(window_, sizes[i]), 0, 0, 0, semibold ? FW_SEMIBOLD : FW_NORMAL, FALSE, FALSE,
                        FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                        DEFAULT_PITCH, face);
    }
    EnumChildWindows(window_, [](HWND h, LPARAM owner) -> BOOL {
        auto& surface = *reinterpret_cast<Surface*>(owner);
        const auto tagged = ULONG_PTR(GetPropW(h, L"ClipRope.FontRole"));
        if (tagged && tagged <= surface.fonts_.size()) surface.font(h, static_cast<Font>(tagged - 1));
        return TRUE;
    }, reinterpret_cast<LPARAM>(this));
    for (auto f : old)
        if (f)
            DeleteObject(f);
    for (auto b : brushes_)
        if (b)
            DeleteObject(b);
    brushes_ = {CreateSolidBrush(colors.window), CreateSolidBrush(colors.panel)};
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}
namespace {
// Live HWND subclasses own this cache. Do not retain apartment objects until TLS
// destruction, which runs after the host has called OleUninitialize.
thread_local IAccPropServices* accessible_services{};
thread_local unsigned accessible_windows{};
LRESULT CALLBACK clear_accessible_properties(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
    if (m == WM_NCDESTROY) {
        auto* properties = reinterpret_cast<IAccPropServices*>(data);
        const MSAAPROPID ids[]{PROPID_ACC_NAME, PROPID_ACC_DESCRIPTION};
        properties->ClearHwndProps(h, DWORD(OBJID_CLIENT), CHILDID_SELF, ids, 2);
        RemoveWindowSubclass(h, clear_accessible_properties, 1);
        if (!--accessible_windows) accessible_services = nullptr;
        properties->Release();
    }
    return DefSubclassProc(h, m, w, l);
}
void accessible_property(HWND h, MSAAPROPID id, const std::wstring& label) {
    Microsoft::WRL::ComPtr<IAccPropServices> properties = accessible_services;
    if (!properties && FAILED(CoCreateInstance(CLSID_AccPropServices, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&properties))))
        return;
    if (FAILED(properties->SetHwndPropStr(h, DWORD(OBJID_CLIENT), CHILDID_SELF, id, label.c_str())))
        return;
    DWORD_PTR existing{};
    if (GetWindowSubclass(h, clear_accessible_properties, 1, &existing)) return;
    if (SetWindowSubclass(h, clear_accessible_properties, 1, DWORD_PTR(properties.Get()))) {
        accessible_services = properties.Get();
        ++accessible_windows;
        properties->AddRef();
    }
    else
        properties->ClearHwndProps(h, DWORD(OBJID_CLIENT), CHILDID_SELF, &id, 1);
}
} // namespace
void accessible_name(HWND h, UiText label) {
    accessible_property(h, PROPID_ACC_NAME, label);
    bind_ui_accessibility(h, std::move(label), false);
}
void accessible_description(HWND h, UiText description) {
    accessible_property(h, PROPID_ACC_DESCRIPTION, description);
    bind_ui_accessibility(h, std::move(description), true);
    NotifyWinEvent(EVENT_OBJECT_DESCRIPTIONCHANGE, h, OBJID_CLIENT, CHILDID_SELF);
}
void Surface::field(HWND h, bool inset) {
    if (!h)
        return;
    font(h);
    if (inset) SetPropW(h, L"ClipRope.InsetField", HANDLE(1));
    SetWindowLongPtrW(h, GWL_STYLE, GetWindowLongPtrW(h, GWL_STYLE) & ~WS_BORDER);
    SetWindowSubclass(h, edit_frame, 1, DWORD_PTR(this));
    SetWindowPos(h, nullptr, 0, 0, 0, 0,
                 SWP_NOSIZE | SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
void Surface::font(HWND child, Font role) {
    SetPropW(child, L"ClipRope.FontRole", HANDLE(UINT_PTR(role) + 1));
    SendMessageW(child, WM_SETFONT, WPARAM(get(role)), FALSE);
}
void Surface::style(HWND h, ButtonStyle opts) {
    DWORD_PTR data{};
    if (GetWindowSubclass(h, button_proc, 1, &data))
        reinterpret_cast<StyledButton*>(data)->options = opts;
    else {
        wchar_t cls[32]{};
        GetClassNameW(h, cls, int(std::size(cls)));
        auto item = std::make_unique<StyledButton>(StyledButton{this, opts, _wcsicmp(cls, L"Button") == 0});
        if (SetWindowSubclass(h, button_proc, 1, DWORD_PTR(item.get())))
            item.release();
    }
    font(h);
    InvalidateRect(h, nullptr, FALSE);
}
void Surface::draw_text(HDC dc, RECT r, std::wstring_view s, Font f, COLORREF c, UINT flags) const {
    auto old = SelectObject(dc, get(f));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, c);
    DrawTextW(dc, s.data(), int(s.size()), &r, flags);
    SelectObject(dc, old);
}
LRESULT Surface::control_color(HDC dc, HWND, bool panel) const {
    SetBkColor(dc, panel ? colors.panel : colors.window);
    SetTextColor(dc, colors.text);
    return reinterpret_cast<LRESULT>(background(panel));
}
} // namespace cliprope::win::fluent
