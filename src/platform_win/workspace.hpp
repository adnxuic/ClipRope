#pragma once
#include "application/preview_changes.hpp"
#include "presentation/color_preview.hpp"
#include "platform_win/hotkey.hpp"

#include "presentation/viewport.hpp"
#include "application/card_search.hpp"
#include "platform_win/inline_editor_host.hpp"
#include "platform_win/workspace_commands.hpp"
#include "platform_win/screenshot_hover.hpp"
#include <windows.h>
#include <functional>
#include <memory>

namespace cliprope::win {
class CardPressInput;

using WorkspaceReads = presentation::ViewportReads;
using DropCompleted = std::function<void(domain::Id)>;

struct WorkspaceActions {
    const presentation::ColorPreview* colors{};
    ScreenshotHover* screenshot_hover{};
    std::function<void(const domain::Entry&, domain::CardScale, const domain::Layout*)> resize;
    InlineEditorHost* editor{};
    std::function<void(POINT)> find;
    std::function<void()> find_scope_changed;
    std::function<void(const domain::Id&)> edit;
    std::function<void(application::CardSummary)> external_drag;
    std::function<void(const std::string &)> press, double_click;
    std::function<void(const std::string &, std::size_t, DropCompleted)> reorder;
    std::function<void(const domain::Id&, const domain::Id&)> erase;
    std::function<void()> hide;
    std::function<void(const domain::Id &)> head;
    std::function<void()> error;
    std::function<void(DWORD)> shortcut_error;
    std::function<void()> cancel_paste;
    std::function<void(HWND)> lost_focus;
    // Called after the window has received the frame and alpha map for a new layout.
    std::function<void()> layout_changed;
    // Release pixel and normalized grab point on the sheet.
    std::function<void(const domain::Id &, POINT, float, float)> detach;
    std::function<void(application::CardMenuSnapshot, POINT)> context;
    std::function<void(const domain::Id &)> select;
    std::function<void(bool)> set_rebin_visible;
    std::function<double(const domain::Id &)> rope_top;
    std::function<void(const domain::Id &, double)> save_rope_top;
    std::function<void(const domain::Id &, const domain::Id &, const domain::Id &, DropCompleted)> reorder_rope;
    std::function<WorkspaceCommand(domain::Rope, POINT, WorkspaceMenuState)> rope_menu;
    std::function<void(const domain::Id &, const domain::Id &, bool copy, std::size_t index, DropCompleted)> transfer;
};

class Workspace final {
  public:
    Workspace(WorkspaceReads reads, WorkspaceActions actions);
    ~Workspace();
    Workspace(const Workspace &) = delete;
    Workspace &operator=(const Workspace &) = delete;
    bool open(); // Create hidden. Call on a per-monitor-V2 UI thread.
    void show(std::string rope_id);
    void hide();
    void always_on_top(bool enabled);
    void shortcuts(const ScopedKeys& keys);
    std::optional<DWORD> hover_keyboard_status() const;
    void match_color(unsigned rgb);
    void find_highlights(std::wstring term, std::optional<application::SearchHit> hit = {});
    bool reveal_match(const application::SearchHit& hit);
    application::FindScope find_scope() const;
    void preview_changed(const application::PreviewChanges& changes);
    void repaint_colors(); // Render overrides only; no viewport reload or anchor persistence.
    void refresh(); // A committed change on the current rope.
    void retranslate(); // Labels only: preserve gestures, drafts and viewport anchors.
    void edit_text(application::TextDraft draft, InlineEditorHost::Save save);
    void editor_changed();
    void display_changed();
    bool visible() const;
    HWND window() const;
    std::shared_ptr<CardPressInput> track_card_press();
    void image_settings(domain::ImagePreferences prefs);
    unsigned picture_width(const domain::Id& rope) const;
    std::size_t realized_count() const;
    std::uint64_t painted_frames() const;
    bool prepare_selection(const domain::Id& rope);
    void set_active(domain::Id rope);
    void set_overview(bool enabled);
    bool in_overview() const;
    // Visible rope-line bounds in physical screen coordinates.
    std::vector<std::pair<domain::Id, RECT>> rows() const;
    RECT card_screen_rect(std::size_t zero_based_index) const;
    std::optional<domain::Id> rope_at(POINT screen_point) const;
    std::optional<presentation::RopeSlot> preview_floating(POINT screen_point, const application::CardSummary* card = nullptr);
    // A successful drop immediately refreshes the committed order itself.
    void clear_floating_preview(bool repaint = true);
    void settle_floating_drop(const domain::Id& landed, const presentation::RopeSlot& slot);
    std::vector<std::pair<domain::Id, RECT>> slots() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace cliprope::win
