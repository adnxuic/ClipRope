#include "platform_win/color_picker.hpp"
#include "platform_win/fluent_tooltip.hpp"
#include "platform_win/card_menu_frame.hpp"
#include "platform_win/color_axis.hpp"
#include "platform_win/fluent_scroll.hpp"
#include "platform_win/menu_timing.hpp"
#include <charconv>
#include <commctrl.h>
#include <windowsx.h>

namespace cliprope::win {
using namespace fluent;
using localization::TextKey;
namespace {
thread_local HWND active_picker{};
constexpr int preset_base = 4100, current = 4121, original = 4122;
constexpr int sv = 4140, hue = 4141, alpha_axis = 4142, hex_input = 4150, rgb_input = 4151;
COLORREF native(domain::ColorRGBA8 c) { return RGB(c.r, c.g, c.b); }
std::wstring hex(domain::ColorRGBA8 c, bool alpha = false) {
    const auto s = domain::color_hex(c, alpha);
    return {s.begin(), s.end()};
}
std::string ascii(std::wstring_view text) {
    std::string result;
    for (auto c : text) {
        if (c > 127)
            return {};
        result += static_cast<char>(c);
    }
    return result;
}
class Picker {
  public:
    ColorPickerOptions options;
    presentation::ColorPickerController controller;
    PendingControlSave saving;
    Surface theme, content;
    CardMenuFrame frame;
    ScrollBar scroll;
    HWND window{}, owner{}, anchor{}, error{}, body{};
    Tooltip tips;
    bool done{}, applied{}, syncing{}, valid_input{true};
    int offset{}, maximum{}, height{570};
    std::array<std::wstring, 8> preset_names;
    std::wstring preview_hint;
    Picker(ColorPickerOptions value)
        : options(std::move(value)), controller(options.value, options.default_color, options.callbacks) {}
    HWND control(int id) const {
        if (auto h = GetDlgItem(window, id))
            return h;
        return GetDlgItem(body, id);
    }
    HWND button(HWND parent, int id, UiText text) {
        auto h = child(parent, L"BUTTON", text, WS_TABSTOP, id);
        (parent == window ? theme : content)
            .card_style(h, {CardPart::picker_action, 0, text, {}, {}, 0, id == IDOK, false, true});
        return h;
    }
    HWND label(HWND parent, int id, UiText text, Font font = Font::menu, bool right = false) {
        auto h = child(parent, L"STATIC", text, SS_NOPREFIX | (right ? SS_RIGHT : 0), id);
        (parent == window ? theme : content).font(h, font);
        return h;
    }
    void create() {
        theme.attach(window);
        label(window, 4123, tr(TextKey::color_title), Font::menu_title);
        auto close = button(window, dialog_close_id, tr(TextKey::common_close));
        theme.card_style(close, {CardPart::dialog_close, 0, {}, {}, {}, 0, false, false, true});
        body = CreateWindowExW(WS_EX_CONTROLPARENT, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                               0, 0, 1, 1, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetWindowSubclass(body, body_proc, 1, DWORD_PTR(this));
        content.attach(body);
        auto swatch = label(body, current, tr(TextKey::color_current));
        SetWindowSubclass(swatch, preview_proc, 1, DWORD_PTR(this));
        label(body, 4130, tr(TextKey::color_current), Font::caption);
        label(body, 4124, L"", Font::menu_title);
        label(body, 4131, tr(TextKey::color_original), Font::caption, true);
        button(body, original, tr(TextKey::color_original));
        label(body, 4132, tr(TextKey::color_presets), Font::caption_bold);
        for (int i = 0; i < 8; ++i) {
            auto h = button(body, preset_base + i, L"");
            SetWindowLongPtrW(h, GWL_STYLE,
                              (GetWindowLongPtrW(h, GWL_STYLE) & ~BS_TYPEMASK) | BS_RADIOBUTTON |
                                  (i == 0 ? WS_GROUP : 0));
            tips.bind(window,h,[this,i] { return UiText(preset_names[i]); });
        }
        tips.bind(window,control(current),[this] { return UiText(preview_hint); });
        label(body, 4134, tr(TextKey::color_custom), Font::caption_bold);
        create_color_axis(body, sv, ColorAxis::sv,
                          tr(TextKey::color_saturation) + L" / " + tr(TextKey::color_value),
                          [this](double s, double v) {
                              auto hsv = controller.hsv();
                              hsv.s = s / 100;
                              hsv.v = v / 100;
                              controller.hsv(hsv);
                              sync();
                          });
        label(body, 4145, tr(TextKey::color_hue));
        label(body, 4147, L"", Font::caption, true);
        create_color_axis(body, hue, ColorAxis::hue, tr(TextKey::color_hue), [this](double h, double) {
            auto hsv = controller.hsv();
            hsv.h = h;
            controller.hsv(hsv);
            sync();
        });
        if (options.allow_alpha) {
            label(body, 4146, tr(TextKey::color_alpha));
            label(body, 4148, L"", Font::caption, true);
            create_color_axis(body, alpha_axis, ColorAxis::alpha, tr(TextKey::color_alpha),
                              [this](double value, double) {
                                  auto color = controller.color();
                                  color.a = std::uint8_t(std::lround(value * 255 / 100));
                                  controller.preview(color);
                                  sync();
                              });
        }
        label(body, 4155, L"HEX", Font::caption);
        label(body, 4156, L"RGB", Font::caption);
        for (int id : {hex_input, rgb_input}) {
            auto h = child(body, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, id);
            content.field(h, true);
            content.font(h, Font::menu);
            accessible_name(h, id == hex_input ? UiText(L"HEX") : UiText(L"RGB"));
            SendMessageW(h, EM_SETLIMITTEXT, id == hex_input ? 9 : 32, 0);
        }
        button(window, IDCANCEL, tr(TextKey::common_cancel));
        button(window, IDOK, tr(TextKey::color_apply));
        error = label(window, 4133, L"", Font::caption);
        scroll.create(window, body, theme.colors, true, [this](int n) {
            offset = n;
            layout();
        });
        layout();
        sync();
    }
    int desired_height() const { return options.allow_alpha ? 570 : 526; }
    int view_height() const { return std::max(1, height - 102); }
    void position() {
        RECT r{};
        GetWindowRect(anchor, &r);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
        auto dpi = design_dpi(GetDpiForWindow(window));
        int w = std::min<int>(MulDiv(336, dpi, 96), mi.rcWork.right - mi.rcWork.left);
        int h = std::min<int>(MulDiv(desired_height(), dpi, 96), mi.rcWork.bottom - mi.rcWork.top - 8);
        height = MulDiv(h, 96, dpi);
        int gap = MulDiv(4, dpi, 96), x = std::clamp<int>(r.left, mi.rcWork.left, mi.rcWork.right - w),
            y = r.bottom + gap;
        if (y + h > mi.rcWork.bottom)
            y = r.top - gap - h;
        y = std::clamp<int>(y, mi.rcWork.top, mi.rcWork.bottom - h);
        SetWindowPos(window, nullptr, x, y, w, h, SWP_NOACTIVATE | SWP_NOZORDER);
        layout();
        frame.fit(window, theme.colors, dpi, 12);
    }
    void layout() {
        if (!body)
            return;
        ChildLayout outer, inner;
        outer.place(control(4123), 20, 16, 248, 24);
        outer.place(control(dialog_close_id), 292, 10, 32, 28);
        const int shift = options.allow_alpha ? 44 : 0, view = view_height();
        maximum = std::max(0, 424 + shift - view);
        offset = std::clamp(offset, 0, maximum);
        outer.place(body, 0, 48, 336, view);
        auto at = [&](int id, int x, int y, int w, int h, bool visible = true) {
            inner.place(control(id), x, y - offset, w, h, visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
        };
        at(current, 20, 2, 40, 38);
        at(4130, 70, 5, 166, 14);
        at(4124, 70, 20, 166, 22);
        at(4131, 238, 5, 78, 14);
        at(original, 264, 24, 52, 16);
        at(4132, 20, 64, 296, 18);
        for (int i = 0; i < 8; ++i)
            at(preset_base + i, 14 + i * 39, 84, 32, 32, i < int(options.presets.size() + 1));
        at(4134, 20, 136, 296, 18);
        at(sv, 20, 158, 296, 146);
        at(4145, 20, 312, 200, 14);
        at(4147, 240, 312, 76, 14);
        at(hue, 13, 326, 310, 32);
        at(4146, 20, 356, 200, 14, options.allow_alpha);
        at(4148, 240, 356, 76, 14, options.allow_alpha);
        at(alpha_axis, 13, 370, 310, 32, options.allow_alpha);
        at(4155, 20, 362 + shift, 150, 18);
        at(4156, 186, 362 + shift, 130, 18);
        at(hex_input, 20, 382 + shift, 150, 32);
        at(rgb_input, 186, 382 + shift, 130, 32);
        outer.place(error, 20, height - 43, 140, 32);
        outer.place(control(IDCANCEL), 168, height - 42, 68, 32);
        outer.place(control(IDOK), 244, height - 42, 72, 32);
        outer.place(scroll.window(), 322, 48, 12, view);
        outer.apply(window);
        inner.apply(body);
        scroll.update(offset, maximum, view, 24);
        RedrawWindow(body, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        InvalidateRect(window, nullptr, FALSE);
    }
    void note(UiText visible, UiText detail = {}) {
        set_ui_text(error, visible);
        accessible_description(error, detail.empty() ? visible : detail);
    }
    void sync(int editing = 0) {
        syncing = true;
        valid_input = true;
        auto color = controller.color();
        InvalidateRect(control(current), nullptr, FALSE);
        accessible_name(control(current), tr(TextKey::color_current) + L" " + hex(color, color.a != 255));
        set_ui_text(control(4124),
                    controller.draft().is_default() ? tr(TextKey::color_default) : UiText(hex(color)));
        const auto old = controller.original().resolve(options.default_color);
        content.card_style(control(original),
                           {CardPart::picker_preview,
                            0,
                            {},
                            {},
                            {},
                            native(domain::ColorRGBA8::rgb(domain::composite(old, options.default_color)))});
        accessible_name(control(original), tr(TextKey::color_original) + L" " + hex(old, old.a != 255));
        for (int i = 0; i < 8; ++i) {
            auto h = control(preset_base + i);
            if (i >= int(options.presets.size() + 1)) {
                ShowWindow(h, SW_HIDE);
                continue;
            }
            auto value = i ? domain::ColorSelection(options.presets.at(i - 1)) : domain::ColorSelection{};
            const bool selected = value == controller.draft();
            content.card_style(h, {CardPart::picker_swatch,
                                   0,
                                   {},
                                   {},
                                   {},
                                   native(value.resolve(options.default_color)),
                                   selected});
            SendMessageW(h, BM_SETCHECK, selected ? BST_CHECKED : BST_UNCHECKED, 0);
            auto name = i ? UiText(hex(value.resolve(options.default_color))) : tr(TextKey::color_default);
            preset_names[i] = name;
            accessible_name(h, name);
        }
        for (int id : {sv, hue, alpha_axis})
            if (auto h = control(id))
                update_color_axis(h, controller.hsv(), color);
        if (editing != hex_input)
            set_ui_text(control(hex_input), hex(color, options.allow_alpha && color.a != 255));
        if (editing != rgb_input)
            set_ui_text(control(rgb_input), std::to_wstring(color.r) + L", " + std::to_wstring(color.g) +
                                                L", " + std::to_wstring(color.b));
        wchar_t value[24];
        swprintf_s(value, L"%.0f°", controller.hsv().h);
        set_ui_text(control(4147), value);
        if (options.allow_alpha) {
            swprintf_s(value, L"%.0f%%", color.a * 100. / 255);
            set_ui_text(control(4148), value);
        }
        UiText warning, detail;
        if (options.contrast_background || options.contrast_ink) {
            double ratio = domain::contrast(
                options.contrast_ink.value_or(color.rgb()),
                options.contrast_background.value_or(domain::composite(color, domain::paper_default)));
            swprintf_s(value, L"%.2f", ratio);
            const bool low = ratio < (options.large_text ? 3 : 4.5);
            detail = message(low ? TextKey::color_low_contrast : TextKey::color_contrast, {{"ratio", value}});
            if (low)
                warning = detail;
            if (options.translucent)
                detail += L"\r\n" + tr(TextKey::color_translucent);
            accessible_description(control(current), detail);
        }
        preview_hint = detail;
        note(warning, detail);
        EnableWindow(control(IDOK), TRUE);
        syncing = false;
    }
    void input(int id) {
        if (syncing)
            return;
        std::optional<domain::ColorRGBA8> color;
        if (id == hex_input)
            color = domain::parse_color(ascii(caption(control(id))), options.allow_alpha);
        else {
            const auto value = ascii(caption(control(rgb_input)));
            std::string_view rest = value;
            int channels[3]{};
            bool valid = true;
            for (int i = 0; i < 3; ++i) {
                const auto comma = rest.find(',');
                auto part = rest.substr(0, comma);
                while (!part.empty() && part.front() == ' ')
                    part.remove_prefix(1);
                while (!part.empty() && part.back() == ' ')
                    part.remove_suffix(1);
                if (part.empty()) {
                    valid = false;
                    break;
                }
                auto parsed = std::from_chars(part.data(), part.data() + part.size(), channels[i]);
                valid &= !part.empty() && parsed.ec == std::errc{} &&
                         parsed.ptr == part.data() + part.size() && channels[i] >= 0 && channels[i] <= 255 &&
                         ((i < 2) == (comma != std::string_view::npos));
                rest = comma == std::string_view::npos ? std::string_view{} : rest.substr(comma + 1);
            }
            if (valid)
                color = domain::ColorRGBA8{std::uint8_t(channels[0]), std::uint8_t(channels[1]),
                                           std::uint8_t(channels[2]), controller.color().a};
        }
        valid_input = color.has_value();
        EnableWindow(control(IDOK), valid_input);
        if (!color) {
            note(tr(TextKey::color_invalid));
            return;
        }
        controller.preview(*color);
        sync(id);
    }
    void refresh_theme() {
        const auto hex_text = caption(control(hex_input)), rgb_text = caption(control(rgb_input));
        const bool incomplete = !valid_input;
        theme.refresh();
        content.refresh();
        sync();
        if (incomplete) {
            syncing = true;
            set_ui_text(control(hex_input), hex_text);
            set_ui_text(control(rgb_input), rgb_text);
            syncing = false;
            valid_input = false;
            EnableWindow(control(IDOK), FALSE);
            note(tr(TextKey::color_invalid));
        }
        frame.fit(window, theme.colors, design_dpi(GetDpiForWindow(window)), 12);
    }
    void command(int id, int notification) {
        if (notification == EN_CHANGE && (id == hex_input || id == rgb_input)) {
            input(id);
            return;
        }
        if (notification == EN_SETFOCUS || notification == BN_SETFOCUS) {
            auto h = control(id);
            if (GetParent(h) == body) {
                RECT r;
                GetWindowRect(h, &r);
                MapWindowPoints(nullptr, body, reinterpret_cast<POINT *>(&r), 2);
                int y = dip(body, r.top), bottom = dip(body, r.bottom), view = view_height();
                if (y < 0)
                    offset = std::max(0, offset + y);
                else if (bottom > view)
                    offset = std::min(maximum, offset + bottom - view);
                else
                    return;
                layout();
            }
            return;
        }
        if (notification != BN_CLICKED)
            return;
        if (id == IDCANCEL || id == dialog_close_id) {
            saving.close();
            done = true;
            return;
        }
        if (saving.pending()) return;
        if (id == original) {
            controller.preview(controller.original());
            sync();
            return;
        }
        if (id >= preset_base && id < preset_base + 8) {
            const int i = id - preset_base;
            if (i > int(options.presets.size()))
                return;
            controller.preview(i ? domain::ColorSelection(options.presets.at(i - 1))
                                 : domain::ColorSelection{});
            sync();
            return;
        }
        if (id == IDOK && valid_input) {
            const auto submitted = controller.draft();
            const bool queued = saving.submit(options.defer_history, [this, submitted] {
                if (done || !IsWindow(owner) || controller.draft() != submitted)
                    throw std::runtime_error("Color edit was invalidated");
                controller.commit();
            }, [this](auto error) {
                if (!error) { applied = done = true; return; }
                EnableWindow(body, TRUE);
                EnableWindow(control(IDOK), valid_input);
                note(tr(TextKey::color_save_failed_short), tr(TextKey::color_save_failed));
            }, true);
            if (queued) {
                EnableWindow(body, FALSE);
                EnableWindow(control(IDOK), FALSE);
                note(tr(TextKey::common_saving));
            }
        }
    }
    void separator(HDC dc, int y) {
        RECT line{px(window, 20), px(window, y), px(window, 316), px(window, y) + 1};
        solid(dc, line, theme.colors.line);
    }
    static LRESULT CALLBACK preview_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        auto p = reinterpret_cast<Picker *>(data);
        if (m == WM_PAINT) {
            PAINTSTRUCT ps;
            auto dc = BeginPaint(h, &ps);
            RECT r;
            GetClientRect(h, &r);
            const auto c =
                domain::ColorRGBA8::rgb(domain::composite(p->controller.color(), p->options.default_color));
            p->content.draw_card(h, dc, r, {CardPart::picker_preview, 0, {}, {}, {}, native(c)});
            EndPaint(h, &ps);
            return 0;
        }
        return DefSubclassProc(h, m, w, l);
    }
    static LRESULT CALLBACK body_proc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR data) {
        auto p = reinterpret_cast<Picker *>(data);
        if (m == WM_COMMAND) {
            p->command(LOWORD(w), HIWORD(w));
            return 0;
        }
        if (m == WM_MOUSEWHEEL) {
            p->scroll.wheel(w);
            return 0;
        }
        if (m == WM_CTLCOLOREDIT) {
            auto dc = HDC(w);
            const auto fill = inset_surface(p->content.colors);
            SetTextColor(dc, p->content.colors.text);
            SetBkColor(dc, fill);
            SetDCBrushColor(dc, fill);
            return LRESULT(GetStockObject(DC_BRUSH));
        }
        if (m == WM_CTLCOLORSTATIC) {
            auto result = p->content.control_color(HDC(w), HWND(l), true);
            const int id = GetDlgCtrlID(HWND(l));
            if (id == 4130 || id == 4131 || id == 4147 || id == 4148 || id == 4155 || id == 4156)
                SetTextColor(HDC(w), p->content.colors.muted);
            return result;
        }
        if (m == WM_ERASEBKGND)
            return 1;
        if (m == WM_PAINT) {
            PAINTSTRUCT ps;
            auto dc = BeginPaint(h, &ps);
            RECT r;
            GetClientRect(h, &r);
            FillRect(dc, &r, p->content.background(true));
            p->separator(dc, 54 - p->offset);
            p->separator(dc, 126 - p->offset);
            EndPaint(h, &ps);
            return 0;
        }
        return DefSubclassProc(h, m, w, l);
    }
    static LRESULT CALLBACK proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        auto p = reinterpret_cast<Picker *>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (m == WM_NCCREATE) {
            p = static_cast<Picker *>(reinterpret_cast<CREATESTRUCTW *>(l)->lpCreateParams);
            p->window = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(p));
        }
        if (!p)
            return DefWindowProcW(h, m, w, l);
        switch (m) {
        case WM_CREATE:
            p->create();
            return 0;
        case WM_CLOSE:
            p->done = true;
            return 0;
        case WM_COMMAND:
            p->command(LOWORD(w), HIWORD(w));
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE && !IsChild(h, HWND(l)))
                p->done = true;
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            auto dc = BeginPaint(h, &ps);
            RECT r;
            GetClientRect(h, &r);
            FillRect(dc, &r, p->theme.background(true));
            p->separator(dc, p->height - 52);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORSTATIC:
            return p->theme.control_color(HDC(w), HWND(l), true);
        case WM_MOUSEWHEEL:
            p->scroll.wheel(w);
            return 0;
        case language_changed:
            refresh_ui_bindings(h);
            [[fallthrough]];
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            p->refresh_theme();
            return 0;
        case WM_DPICHANGED:
            p->refresh_theme();
            p->position();
            return 0;
        }
        return DefWindowProcW(h, m, w, l);
    }
};
} // namespace
void color_picker_button(Surface &surface, HWND button, domain::ColorSelection color, unsigned fallback,
                         ColorButtonStyle style, bool alpha) {
    auto resolved = color.resolve(fallback);
    const UiText label = color.is_default() && style != ColorButtonStyle::settings
                             ? tr(TextKey::color_default) : UiText(hex(resolved, alpha));
    accessible_name(button, style == ColorButtonStyle::settings
                                ? message(TextKey::settings_color_accessible, {{"color", label}}) : label);
    if (alpha)
        resolved = domain::ColorRGBA8::rgb(domain::composite(resolved, fallback));
    surface.card_style(button, {CardPart::color_picker,
                                0,
                                style == ColorButtonStyle::compact ? UiText{} : label,
                                {},
                                {},
                                native(resolved),
                                false,
                                false,
                                style != ColorButtonStyle::settings});
}
bool ColorPickerPopup::active() { return active_picker != nullptr; }
bool ColorPickerPopup::owns(HWND h) {
    return h && active_picker && (h == active_picker || IsChild(active_picker, h));
}
void ColorPickerPopup::dismiss() {
    if (active_picker)
        SendMessageW(active_picker, WM_CLOSE, 0, 0);
}
bool ColorPickerPopup::track(HWND owner, HWND anchor, ColorPickerOptions options) {
    if (active_picker) {
        dismiss();
        return false;
    }
    menu_mark(MenuStage::input);
    Picker picker(std::move(options));
    picker.owner = owner;
    picker.anchor = anchor;
    menu_mark(MenuStage::model);
    WNDCLASSW wc{};
    wc.lpfnWndProc = Picker::proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ClipRope.ColorPicker";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return false;
    auto parent = picker.frame.open(owner) ? picker.frame.window() : owner;
    menu_mark(MenuStage::focus);
    auto h = create_ui_window(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_CONTROLPARENT, wc.lpszClassName,
                              tr(TextKey::color_title), WS_POPUP | WS_CLIPCHILDREN, 0, 0, 336, 570, parent,
                              nullptr, wc.hInstance, &picker);
    if (!h)
        return false;
    menu_mark(MenuStage::controls);
    active_picker = h;
    picker.position();
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(picker.control(preset_base));
    menu_mark(MenuStage::frame);
    RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    menu_mark(MenuStage::painted);
    MSG msg{};
    while (!picker.done && IsWindow(owner) && IsWindow(h)) {
        int result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            if (!result)
                PostQuitMessage(int(msg.wParam));
            break;
        }
        if (!picker.controller.valid()) {
            picker.done = true;
            break;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
            picker.done = true;
            continue;
        }
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_RETURN) {
            int id = GetDlgCtrlID(msg.hwnd);
            if (id == hex_input || id == rgb_input) {
                picker.input(id);
                continue;
            }
        }
        if (!owns(msg.hwnd) && (msg.message == WM_LBUTTONDOWN || msg.message == WM_RBUTTONDOWN ||
                                msg.message == WM_NCLBUTTONDOWN)) {
            picker.done = true;
            continue;
        }
        if (!IsDialogMessageW(h, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!picker.controller.valid())
            picker.done = true;
    }
    picker.saving.close();
    picker.controller.cancel();
    if (GetForegroundWindow() == h && IsWindow(owner)) {
        SetForegroundWindow(owner);
        if (IsWindow(anchor))
            SetFocus(anchor);
    }
    active_picker = nullptr;
    DestroyWindow(h);
    return picker.applied;
}
} // namespace cliprope::win
