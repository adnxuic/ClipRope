#pragma once
#include "platform_win/fluent_tooltip.hpp"
#include "platform_win/defer_history_command.hpp"
#include "application/card_preferences.hpp"
#include "application/appearance.hpp"
#include "platform_win/hotkey.hpp"
#include "platform_win/fluent_scroll.hpp"
#include "localization/localization.hpp"
#include <filesystem>
#include <future>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace cliprope::win {
enum class ShortcutState { registered, available, off, unverified, limited, unavailable, save_failed };
struct ShortcutResult {
    ShortcutState state{};
    UiText description;
    bool operator==(const ShortcutResult&) const = default;
};

struct SettingsSnapshot {
    localization::Language language = localization::Language::automatic;
    std::function<void(localization::Language)> save_language;
    bool topmost = true;
    std::optional<bool> launch_at_sign_in;
    UiText startup_notice;
    std::function<void(SettingsSnapshot&)> refresh_settings;
    std::function<void(bool)> save_startup, save_topmost;
    using Exclusions = std::vector<std::string>;
    std::function<Exclusions()> read_exclusions;
    std::function<void(const Exclusions&, Exclusions)> save_exclusions;
    std::function<void()> reset_settings;
    application::CardPreferences cards;
    std::function<void(const application::CardPreferences&)> save_cards;
    std::function<void(int)> save_capacity;
    application::Appearance appearance;
    std::function<void(application::Appearance)> save_appearance;
    std::function<void(std::optional<domain::ColorSelection>)> preview_match;
    std::function<void(domain::ColorSelection,domain::ColorSelection)> save_match;
    std::filesystem::path profile, pending;
    UiText location_notice;
    std::function<void(const std::filesystem::path&)> schedule;
    std::function<void()> restart;
    std::function<std::uint64_t()> measure;
    int default_capacity = 100;
    std::function<application::ShortcutBinding(application::ShortcutAction)> binding;
    std::function<ShortcutResult(application::ShortcutAction)> status;
    // An empty candidate checks the saved binding and retries a failed registration.
    std::function<ShortcutResult(application::ShortcutAction, std::optional<application::ShortcutBinding>)> configure;
    DeferHistoryCommand defer_history;
};

class Settings final {
  public:
    ~Settings();
    Settings() = default;
    Settings(const Settings&) = delete;
    Settings& operator=(const Settings&) = delete;
    void open(SettingsSnapshot data, bool shortcut_conflict = false);
    bool dialog_message(MSG& msg);
    void close();

  private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    void page(int index);
    void submit_setting(HWND detail, std::function<void()> write, std::function<void(std::exception_ptr)> reply);
    PendingControlSave saving_;
    std::optional<int> next_page_;
    void layout();
    enum { card_action = 1920, card_font, card_resolution, card_thumbnails, card_keep_group, rope_capacity,
           card_screenshot_preview, card_follow_head, rope_follow_collector, card_delete_hanging, card_delete_floating };
    static constexpr bool card_choice(int id) {
        return id == card_action || id == card_delete_hanging || id == card_delete_floating;
    }
    static constexpr int card_order[]{card_action, card_follow_head, card_delete_hanging, card_delete_floating,
                                      card_font, card_resolution, card_screenshot_preview,
                                      card_thumbnails, card_keep_group};
    static constexpr int rope_order[]{rope_capacity, rope_follow_collector};
    static std::wstring wide(std::string_view text);
    enum { language = 1940, launch, topmost, reset, exclusions };
    static constexpr int general_order[]{language, topmost, launch, reset};
    void edit_exclusions();
    void general_rows();
    void sync_general();
    void sync_language_choice(bool relabel);
    void retranslate();
    bool general_control(int id, int notification);
    void card_rows();
    bool card_control(int id, int notification);
    void sync_card_control(int id);
    void fill_card_choice(int id);
    void preference_rows();
    bool preference_control(int id, int notification);
    void preference_error(int id);
    void show_preferences();
    void measure_usage();
    std::future<std::uint64_t> measurement_;
    ULONGLONG next_measure_{};
    void shortcut_rows();
    void sync_bindings();
    void show_chord(application::ShortcutAction action);
    void show_check(application::ShortcutAction action, const ShortcutResult* feedback = nullptr);
    void configure_binding(application::ShortcutAction action, std::optional<application::ShortcutBinding> binding, HWND source);
    void show_tip(HWND source, UiText text, bool immediate);
    void close_tip();
    void clear_feedback();
    struct ShortcutView {
        std::optional<application::ShortcutBinding> binding;
        ShortcutResult status;
        UiText description;
        ULONGLONG deadline{};
    };
    std::array<ShortcutView, application::shortcut_count> shortcuts_{};
    HWND tip_{}, tip_source_{};
    fluent::Tooltip tooltip_;
    UiText tip_message_;
    unsigned layout_count_{};
    void begin_recording(application::ShortcutAction action);
    void end_recording();
    bool shortcut_control(int id, int notification);
    static LRESULT CALLBACK recording_hook(int code, WPARAM kind, LPARAM data);
    static thread_local Settings* recorder_;
    HHOOK record_hook_{};
    application::ShortcutAction recording_ = application::shortcut_count;
    Hotkey captured_{};
    UINT recording_session_{};

    static LRESULT CALLBACK viewport_proc(HWND, UINT, WPARAM, LPARAM);
    void position_rows();
    void paint(HDC dc, bool body);
    void scroll(int offset);
    struct Row {
        HWND label{}, detail{}, value{};
        int label_height{}, detail_height{}, text_width{}, control_width{};
        int top{}, height{};
        HWND toggle{}, check{}, defer{}, heading{};
    };
    void heading(Row& row, UiText text);
    SettingsSnapshot data_;
    fluent::Surface surface_;
    std::vector<Row> rows_;
    HWND window_{}, title_{}, viewport_{};
    fluent::ScrollBar scrollbar_;
    static constexpr int margin_ = 28;
    int body_width_{}, view_height_{};
    int selected_{0}, offset_{}, max_offset_{}, sidebar_{}, width_{860}, height_{532};
};
} // namespace cliprope::win
