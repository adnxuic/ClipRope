#pragma once
#include "platform_win/card_menu_frame.hpp"
#include "platform_win/fluent.hpp"
#include "platform_win/defer_history_command.hpp"
#include "platform_win/fluent_tooltip.hpp"
#include <vector>
#include <optional>

namespace cliprope::win {
enum class PopupFace { standard, card, rope, tray };
enum class PopupKind { command, text, number, swatches, opacity, toggle, segments, color };
struct PopupResult {
    UiText error;
    bool dismiss{};
};
struct PopupItem {
    int id;
    UiText label;
    wchar_t icon{};
    bool enabled = true;
    PopupKind kind = PopupKind::command;
    std::wstring value;
    std::function<PopupResult(const std::wstring&)> commit;
    bool divider{}, danger{};
    std::vector<COLORREF> palette;
    bool literal{};
    UiText detail, hint;
    bool square_swatches{}, preferred{}, chevron{};
    std::array<UiText, 2> segments{};
    bool checked{};
    int selection{};
    std::function<PopupResult(bool)> commit_toggle;
    UiText display_value;
    domain::ColorSelection color;
    unsigned color_default{};
    bool color_alpha{};
    std::function<domain::ColorSelection(HWND, HWND)> pick_color;
};
// Strings and callbacks live only for the duration of track. Zero denotes dismissal.
class PopupMenuHost final {
  public:
    using RefreshCommands = std::function<bool(UiText&, std::vector<PopupItem>&)>;
    static void file_preview_ready();
    static int track(HWND owner, POINT at, UiText title, UiText subtitle, std::vector<PopupItem> items,
                     PopupFace face = PopupFace::standard, RefreshCommands refresh = {},
                     void (*return_focus)(HWND owner, int choice) = nullptr, const RECT* exclude = nullptr,
                     DeferHistoryCommand defer_history = {});
    static bool input_active();
    static bool activates(HWND window);

  private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    RECT placement(HWND owner, POINT at, const RECT* exclude = nullptr) const;
    void size_for_labels();
    void refresh_commands();
    RefreshCommands refresh_;
    POINT anchor_{};
    HWND owner_{};
    std::optional<RECT> exclusion_;
    int base_width_{};
    SIZE layout_size_{};
    void create();
    void create_card();
    void layout_card();
    void create_rope();
    void layout_rope();
    void paint_rope(HDC dc);
    int rope_body_height() const;
    void create_tray();
    void layout_tray();
    void paint_tray(HDC dc);
    int tray_height() const;
    bool tray_key(WPARAM key);
    int row_height(const PopupItem&) const;
    int error_height() const;
    void show_field(size_t row);
    void finish_field();
    void style_swatches(size_t row);
    bool card() const { return face_ == PopupFace::card; }
    bool rope() const { return face_ == PopupFace::rope; }
    bool tray() const { return face_ == PopupFace::tray; }
    static constexpr int field_base = 10000, footer = 9000;
    static constexpr int menu_header = 48, menu_bottom = 8, menu_divider = 4;
    static int extra(size_t row, int part) { return 30000 + int(row) * 10 + part; }
    PopupFace face_{};
    CardMenuFrame frame_;
    int editing_id_{};
    void layout();
    void paint(HDC dc);
    void submit(size_t index, std::wstring value);
    void submit_value(size_t index, std::wstring value);
    void submit_toggle(size_t index, bool proposed);
    void write(std::function<void()> commit);
    void update_status();
    DeferHistoryCommand defer_history_;
    PendingControlSave saving_;
    void handle(int id);
    bool key(MSG& msg);
    int preferred_height() const;
    HWND window_{}, status_{};
    fluent::Surface theme_;
    fluent::Tooltip tips_;
    UiText title_, subtitle_, message_;
    std::vector<PopupItem> items_;
    int width_{}, scroll_{}, limit_{}, choice_{};
    bool done_{}, layout_ready_{};
};
} // namespace cliprope::win
