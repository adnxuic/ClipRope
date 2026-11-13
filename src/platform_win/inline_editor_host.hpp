#pragma once
#include "application/card_edit.hpp"
#include "platform_win/localization.hpp"
#include "platform_win/fluent.hpp"
#include <windows.h>
#include <functional>
#include <future>
#include <list>
#include <vector>

namespace cliprope::win {
// One activatable paper inset, shared by the workspace and all Floating cards.
class InlineEditorHost final {
public:
    // Runs on the storage worker and returns the committed snapshot identity.
    using Save = std::function<domain::Id(const application::TextDraft&, std::wstring_view)>;
    using Ended = std::function<void(const application::CardSummary*)>;
    InlineEditorHost() = default;
    ~InlineEditorHost();
    InlineEditorHost(const InlineEditorHost&) = delete;
    InlineEditorHost& operator=(const InlineEditorHost&) = delete;

    void bind(HWND owner, application::TextDraft draft, const application::CardSummary& card,
              Save write, std::function<bool()> present, Ended ended);
    void place(RECT screen, RECT visible, bool floating = false);
    void focus();
    void font_size(double size);
    void colors(const application::CardStyle &style);
    void save(std::function<void()> after = {});
    // Independent requests all run after this edit finishes. A failed save drops them.
    void finish_then(std::function<void()> action);
    void cancel();
    void discard(); // Owner destruction or eviction.
    void drain();
    bool previewing() const { return phase_ != Phase::editing; }
    const application::CardSummary* preview() const { return preview_ ? &*preview_ : nullptr; }
    bool translate(const MSG& message);
    void wheel(WPARAM w);
    std::function<void()> binding_changed;
    std::function<bool(std::function<void()>)> defer_write;
    std::function<void()> command_failed;
    std::function<void(const application::TextDraft&, const domain::Id&)> committed;
    std::function<void(const domain::Id&)> evicted;
    HWND owner() const { return owner_; }
    const domain::Id& entry() const { return entry_.id; }
    RECT inset(HWND owner) const;

private:
    enum class Phase { editing, present_save, present_cancel, writing };
    void handoff();
    void finish(const application::CardSummary* saved);
    void complete(bool wait);
    void restore(UiText message);
    void create();
    void layout();
    void update_notice_font(UINT dpi);
    void format();
    void notice(UiText text);
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK field_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    UINT paper_dpi_{};
    int paper_width_{};
    HWND host_{}, field_{}, owner_{};
    fluent::Surface surface_;
    HMODULE library_{};
    HBRUSH paper_{};
    HFONT notice_font_{};
    COLORREF ink_{}, background_{};
    RECT screen_{}, visible_{};
    std::shared_ptr<const application::TextDraft> draft_;
    domain::Entry entry_;
    std::optional<application::CardSummary> preview_;
    std::wstring submitted_;
    struct Job {
        std::shared_ptr<const application::TextDraft> session;
        std::future<domain::Id> result;
    };
    std::list<Job> jobs_;
    application::CardStyle style_;
    Save write_;
    std::function<bool()> present_;
    Ended ended_;
    std::vector<std::function<void()>> after_;
    int wheel_remainder_{};
    POINT saved_scroll_{};
    LONG selection_start_{}, selection_end_{};
    Phase phase_ = Phase::editing;
    ULONGLONG presentation_deadline_{};
    bool composing_{};
};
}
