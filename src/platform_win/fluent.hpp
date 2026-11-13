#pragma once
#include "application/appearance.hpp"
#include "platform_win/localization.hpp"
#include "platform_win/fluent_icons.hpp"
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace cliprope::win::fluent {
struct Palette {
    COLORREF window, panel, subtle, line, text, muted, accent, danger, on_accent;
    bool dark{}, contrast{};
    bool operator==(const Palette&) const = default;
};
INT_PTR modal(HWND owner, DLGPROC proc, LPARAM data);
bool management_prompt(HWND owner, UiText title, UiText context, UiText message,
                       UiText action, bool confirm, int height);
Palette system_palette();
COLORREF inset_surface(const Palette& colors);
void set_theme(application::Theme mode);
UINT design_dpi(UINT monitor_dpi);
int px(HWND window, int dip);
int dip(HWND window, int pixels);
void place(HWND child, int x, int y, int width, int height);
class ChildLayout final {
  public:
    void move(HWND child, int x, int y, int width, int height, UINT visibility = 0);
    void place(HWND child, int x, int y, int width, int height, UINT visibility = 0);
    void apply(HWND parent, bool repaint_children = false);
  private:
    struct Move { HWND child; int x, y, width, height; UINT flags; };
    std::vector<Move> moves_;
};
std::wstring caption(HWND control);
HWND child(HWND parent, const wchar_t* type, UiText text, DWORD style, int id, bool use_visual_styles = false);
void round_box(HDC dc, RECT rect, int radius, COLORREF fill, COLORREF border, COLORREF backdrop = CLR_INVALID);
void solid(HDC dc, RECT rect, COLORREF color);
enum class Font { body, detail, section, title, icon, menu, menu_title, caption, caption_bold };
enum class Button { normal, primary, danger, navigation, command, swatch, toggle, value };
struct ButtonStyle {
    Button kind = Button::normal;
    wchar_t glyph{};
    bool selected{}, icon_only{};
    COLORREF color{};
    bool literal{};
    UiText display;
};

enum class CardPart {
    header,
    label,
    toggle,
    decrease,
    increase,
    size_value,
    percent_value,
    dot,
    picker_swatch,
    tile,
    slider,
    action,
    rope_header,
    rope_style,
    rope_action,
    band,
    input,
    segment_left,
    segment_middle,
    segment_right,
    color_picker,
    dialog_close, dialog_button, settings_nav, settings_value, settings_choice, settings_switch, panel, brand_icon, dialog_field, folder_choice,
    tray_header, tray_band, tray_preview, tray_quick_note, tray_settings, tray_exit, find_button,
    picker_preview, picker_action, picker_field, icon_button, list_item, paper_action
};
struct CardInk {
    CardPart part;
    wchar_t glyph{};
    UiText label, detail, hint;
    COLORREF color{};
    bool selected{}, on_band{}, on_panel{}, chevron{};
    bool operator==(const CardInk&) const = default;
};
struct PaperInk {
    HBRUSH texture{};
    COLORREF background{}, foreground{};
};

// Each HWND tree owns its native controls. Its UI thread owns the shared drawing resources.
class Surface final {
  public:
    Surface();
    ~Surface();
    Surface(const Surface&) = delete;
    Surface& operator=(const Surface&) = delete;
    void attach(HWND window);
    // Use the client area for the entire dialog, retaining the DWM shell and native input.
    HWND dialog_chrome(HWND window);
    void refresh();
    void release();
    static void discard_target();
    static unsigned target_creations();
    static constexpr size_t artwork_budget = 4u * 1024 * 1024;
    static size_t control_artwork_bytes();
    static void clear_control_artwork();
    // Pixel-space primitive, including an explicit GDI recovery path in round_box.
    static bool rounded(HDC dc, RECT bounds, int radius, COLORREF fill, COLORREF border, COLORREF backdrop);
    void style(HWND button, ButtonStyle options = {});
    void card_style(HWND control, CardInk ink);
    void paper_style(HWND control, Icon icon, std::function<PaperInk()> paper);
    void draw_card(HWND control, HDC dc, RECT bounds, const CardInk& ink, bool hot = false,
                   float hover = -1, float press = -1, const PaperInk* paper = nullptr);
    SIZE measure_paragraph(std::wstring_view text, int max_width);
    void draw_paragraph(HDC dc, RECT bounds, std::wstring_view text, COLORREF ink, COLORREF background);
    void field(HWND edit, bool inset = false);
    void font(HWND child, Font role = Font::body);
    void menu_header(HDC dc, RECT area, std::wstring_view title, std::wstring_view detail, wchar_t glyph);
    void draw_text(HDC dc, RECT bounds, std::wstring_view text, Font font, COLORREF color, UINT flags) const;
    LRESULT control_color(HDC dc, HWND child, bool panel = false) const;
    Palette colors{};
    HFONT get(Font role) const { return fonts_[size_t(role)]; }
    HBRUSH background(bool panel = false) const { return brushes_[panel ? 1 : 0]; }

  private:
    struct Graphics;
    static thread_local std::unique_ptr<Graphics> graphics_;
    bool paint_card(HWND control, HDC dc, RECT area, const CardInk& ink, bool hot,
                    float hover = -1, float press = -1, const PaperInk* paper = nullptr);
    static bool bind(HDC dc, const RECT& area);
    HWND window_{};
    std::array<HFONT, size_t(Font::caption_bold) + 1> fonts_{};
    std::array<HBRUSH, 2> brushes_{};
};
constexpr int dialog_close_id = 1599;
void accessible_name(HWND control, UiText name);
void accessible_description(HWND control, UiText description);
void window_frame(HWND window, const Palette& colors, bool compact_corner = false);
} // namespace cliprope::win::fluent
