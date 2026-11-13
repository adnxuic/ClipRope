#pragma once
#include "application/rope_changes.hpp"
#include "platform_win/defer_history_command.hpp"
#include "platform_win/workspace_commands.hpp"
#include "platform_win/fluent.hpp"
#include <functional>

namespace cliprope::win {
struct RopeActions {
    std::function<int()> default_capacity;
    std::function<std::wstring()> find_hint;
    std::function<domain::Rope(const domain::Id&)> current;
    std::function<void(domain::Id)> select;
    std::function<application::RopePreview(const application::RopeChange &)> preview;
    std::function<std::optional<domain::Id>(const application::RopePreview &)> apply;
    std::function<void(std::string_view, std::optional<domain::ColorSelection>)> preview_color;
    std::function<void(std::string_view, domain::ColorSelection, domain::ColorSelection)> save_color;
    DeferHistoryCommand defer_history;
};
class RopeManager final {
public:
    explicit RopeManager(RopeActions actions);
    ~RopeManager();
    RopeManager(const RopeManager &) = delete;
    RopeManager &operator=(const RopeManager &) = delete;
    WorkspaceCommand menu(HWND owner, POINT screen, const domain::Rope &rope, WorkspaceMenuState state);
    void open(HWND owner, const domain::Rope &rope, application::RopeOp op);
    bool dialog_message(MSG &msg) const;

private:
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    bool controls();
    void layout();
    void accept();
    void review(application::RopePreview preview);
    void show(HWND owner, UiText caption);
    void confirm_switch(HWND owner, domain::Id created);
    void style_actions();
    void enable_inputs(bool enabled);
    void pick_color();
    RopeActions actions_;
    PendingControlSave saving_;
    domain::Rope rope_;
    domain::ColorSelection color_draft_;
    application::RopeOp op_{};
    bool switching_ = false;
    std::optional<application::RopePreview> consent_;
    HWND window_{};
    fluent::Surface theme_;
};
} // namespace cliprope::win
