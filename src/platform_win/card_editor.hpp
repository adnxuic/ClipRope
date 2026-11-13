#pragma once
#include "application/card_edit.hpp"
#include "platform_win/defer_history_command.hpp"
#include <windows.h>
#include <functional>
#include "platform_win/fluent_scroll.hpp"
#include <vector>

namespace cliprope::win {
enum class CardCommand : UINT {
    font = 1801, bold, foreground, background, opacity, edit, information, open, reveal, refresh, expand = 1812
};
struct EditorActions {
    std::function<void(const domain::Entry&)> edit_text;
    std::function<domain::PathKind(const application::CardMenuSnapshot&)> confirm_path_kind;
    std::function<void(std::string_view, const application::CardStyle &, bool)> save_style;
    std::function<void(const domain::Entry &, bool, std::optional<domain::ColorSelection>)> preview_color;
    std::function<void(const domain::Entry &, bool, domain::ColorSelection, domain::ColorSelection)>
        save_color;
    std::function<application::CardDetails(const application::CardSummary&)> details;
    std::function<application::CardSummary(const domain::Entry&)> current;
    std::function<void(CardCommand, const application::CardSummary&, HWND)> file;
    std::function<void(const domain::Entry&, HWND)> expand;
    DeferHistoryCommand defer_history;
};
class CardEditor final {
public:
    explicit CardEditor(EditorActions actions);
    ~CardEditor();
    CardEditor(const CardEditor &) = delete;
    CardEditor &operator=(const CardEditor &) = delete;
    void menu(HWND owner, POINT screen, application::CardMenuSnapshot card);
    void show_information(const application::CardSummary& card);
    void refresh_information();
    static void file_notice(HWND owner, UiText reason, std::wstring path);
    bool dialog_message(MSG &msg) const;

private:
  enum class Mode { information, notice };
  void open(const application::CardSummary& card, Mode mode);
  void show(HWND owner = nullptr);
  void format_details(bool refresh_theme = false);
  void sync_scroll();
  void scroll_to(int position);
  static LRESULT CALLBACK details_proc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  bool controls();
  void layout();

  static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
  EditorActions actions_;
  fluent::Surface theme_;
  fluent::ScrollBar scrollbar_;
  int text_height_{};
  bool formatting_{};
  application::CardDetails details_;
  UiText notice_reason_;
  HMODULE rich_edit_{};
  bool showing_information_{};
  domain::Entry entry_;
  Mode mode_{};
  HWND window_{};

};
} // namespace cliprope::win
