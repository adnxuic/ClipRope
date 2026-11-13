#pragma once
#include "platform_win/settings.hpp"
#include <shellapi.h>
#include <optional>
#include <vector>

namespace cliprope::win {
struct TrayItem { std::string id; std::wstring summary; };
struct TrayModel { std::string rope_id; std::wstring rope; std::vector<TrayItem> items; std::optional<std::string> hidden_card; };
struct TrayRoutes {
    bool icon{}, workspace_hotkey{};
    bool reachable() const { return icon || workspace_hotkey; }
};
struct TrayActions {
    std::function<void()> toggle;
    std::function<void()> open_workspace;
    std::function<void()> create_quick_note;
    std::function<TrayModel()> menu;
    std::function<void(const std::string&, const std::string&)> recall, head;
    std::function<void()> exit;
    std::function<application::ShortcutBindings()> read_shortcuts;
    std::function<void(application::ShortcutAction, const application::ShortcutBinding&)> save_shortcut;
    std::function<void(const ScopedKeys&)> set_scoped;
    std::function<std::optional<DWORD>()> hover_status;
    std::function<SettingsSnapshot()> snapshot;
    std::function<void(const std::string&)> reveal_hidden;
};

class Tray final {
public:
    explicit Tray(TrayActions actions, decltype(&Shell_NotifyIconW) shell = Shell_NotifyIconW,
                 std::optional<bool> visible_station = {});
    ~Tray();
    Tray(const Tray&)=delete;
    Tray& operator=(const Tray&)=delete;
    bool start(HWND listener);
    void reload_shortcuts();
    bool hotkey_enabled() const { return bindings_[application::workspace_toggle].enabled; }
    bool hotkey_registered() const { return registrations_[0] != 0; }
    TrayRoutes routes() const { return {icon_added_, hotkey_registered()}; }
    bool needs_workspace() const { return visible_station_ && !routes().reachable(); }
    std::optional<LRESULT> message(UINT msg,WPARAM w,LPARAM l);
    bool dialog_message(MSG& msg) { return settings_.dialog_message(msg); }
    std::wstring shortcut_hint(application::ShortcutAction action) const;
    void notify(UiText text) const;
    static constexpr UINT callback=WM_APP+150;
    static constexpr UINT icon_id=1;
private:
    void install_icon();
    void recover_icon(bool expose);
    void settings();
    void menu(POINT point);
    void stop();
    ShortcutResult configure(application::ShortcutAction action, std::optional<application::ShortcutBinding> candidate);
    ShortcutResult status(application::ShortcutAction action) const;
    UiText conflict(application::ShortcutAction action, Hotkey chord, bool all) const;
    void update_scoped();
    application::ShortcutBindings bindings_ = application::default_shortcuts();
    std::array<int, 2> registrations_{};
    TrayActions actions_;
    Settings settings_;
    HWND listener_{};
    UINT shell_restart_{};
    bool icon_added_{};
    unsigned retries_{};
    decltype(&Shell_NotifyIconW) shell_;
    bool visible_station_;
};
}
