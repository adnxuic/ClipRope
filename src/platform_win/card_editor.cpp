#include "platform_win/card_editor.hpp"
#include "platform_win/color_picker.hpp"
#include "platform_win/popup_menu.hpp"
#include "presentation/card_labels.hpp"
#include <algorithm>
#include <charconv>
#include <commctrl.h>
#include <richedit.h>
#include <cmath>
#include <cstdio>
#include <optional>
#include <span>

namespace cliprope::win {
namespace {
constexpr wchar_t editor_class[] = L"ClipRope.CardEditor";
constexpr int content = 1701, heading = 1708;
LONG first_visible_char(HWND body) {
    return LONG(SendMessageW(body, EM_LINEINDEX, SendMessageW(body, EM_GETFIRSTVISIBLELINE, 0, 0), 0));
}
void restore_visible_line(HWND body, LONG anchor) {
    if (anchor < 0) return;
    const auto index = std::min<LONG>(anchor, GetWindowTextLengthW(body));
    const auto line = SendMessageW(body, EM_EXLINEFROMCHAR, 0, index);
    const auto first = SendMessageW(body, EM_GETFIRSTVISIBLELINE, 0, 0);
    if (line != first) SendMessageW(body, EM_LINESCROLL, 0, line - first);
}
std::wstring number(double value) {
    char buffer[60];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return {buffer, result.ptr};
}
std::string ascii(std::wstring_view text) {
    std::string value;
    for (auto ch : text) {
        if (ch > 127)
            return {};
        value += static_cast<char>(ch);
    }
    return value;
}
std::optional<double> decimal(const std::string& text, double min, double max) {
    double value{};
    const auto read = std::from_chars(text.data(), text.data() + text.size(), value);
    if (read.ec != std::errc{} || read.ptr != text.data() + text.size() || !std::isfinite(value) || value < min ||
        value > max)
        return {};
    return value;
}
struct MenuItem {
    CardCommand command;
    UiText label;
};

const MenuItem text_menu[] = {
    {CardCommand::font, tr(TextKey::card_font)},         {CardCommand::bold, tr(TextKey::card_bold)},
    {CardCommand::foreground, tr(TextKey::card_text_color)},  {CardCommand::background, tr(TextKey::card_background_color)},
    {CardCommand::opacity, tr(TextKey::card_opacity)},        {CardCommand::edit, tr(TextKey::card_edit)},
    {CardCommand::information, tr(TextKey::card_information_action)}};
const MenuItem screenshot_menu[] = {{CardCommand::open, tr(TextKey::card_open)},
                                        {CardCommand::reveal, tr(TextKey::card_reveal)},
                                        {CardCommand::information, tr(TextKey::card_information_action)}};
const MenuItem file_menu[] = {{CardCommand::open, tr(TextKey::card_open)},
                                  {CardCommand::reveal, tr(TextKey::card_reveal)},
                                  {CardCommand::refresh, tr(TextKey::card_refresh_preview)},
                                  {CardCommand::information, tr(TextKey::card_information_action)}};
const MenuItem group_menu[] = {{CardCommand::reveal, tr(TextKey::card_reveal)},
                                   {CardCommand::refresh, tr(TextKey::card_refresh_preview)},
                                   {CardCommand::expand, tr(TextKey::card_expand_group)},
                                   {CardCommand::information, tr(TextKey::card_information_action)}};
const MenuItem folder_menu[] = {{CardCommand::reveal, tr(TextKey::card_reveal)},
                                    {CardCommand::refresh, tr(TextKey::card_refresh_preview)},
                                    {CardCommand::information, tr(TextKey::card_information_action)}};
const MenuItem other_menu[] = {{CardCommand::information, tr(TextKey::card_information_action)}};
std::span<const MenuItem> items(application::CardMenuKind kind) {
    using enum application::CardMenuKind;
    switch (kind) {
    case text:
        return text_menu;
    case screenshot:
        return screenshot_menu;
    case single_file:
        return file_menu;
    case multiple_files:
        return group_menu;
    case folder:
        return folder_menu;
    default:
        return other_menu;
    }
}
UiText card_type(application::CardMenuKind kind) {
    using enum application::CardMenuKind;
    switch (kind) {
    case text:
        return tr(TextKey::card_kind_text);
    case screenshot:
        return tr(TextKey::card_kind_screenshot);
    case single_file:
        return tr(TextKey::card_kind_file);
    case multiple_files:
        return tr(TextKey::card_kind_group);
    case folder:
        return tr(TextKey::card_kind_folder);
    default:
        return tr(TextKey::card_kind_other);
    }
}
std::wstring rgb_hex(unsigned color) {
    wchar_t label[8]{};
    swprintf_s(label, L"#%06X", color);
    return label;
}
} // namespace
CardEditor::CardEditor(EditorActions actions) : actions_(std::move(actions)) {}
CardEditor::~CardEditor() {
    if (window_)
        DestroyWindow(window_);
    if (rich_edit_) FreeLibrary(rich_edit_);
}
bool CardEditor::dialog_message(MSG& msg) const {
    if (!window_ || (msg.hwnd != window_ && !IsChild(window_, msg.hwnd)))
        return false;
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE) {
        SendMessageW(window_, WM_CLOSE, 0, 0);
        return true;
    }
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_TAB && GetKeyState(VK_CONTROL) >= 0 &&
        GetKeyState(VK_MENU) >= 0) {
        SendMessageW(window_, WM_CHANGEUISTATE, MAKEWPARAM(UIS_CLEAR, UISF_HIDEFOCUS), 0);
        if (auto next = GetNextDlgTabItem(window_, GetFocus(), GetKeyState(VK_SHIFT) < 0))
            SetFocus(next);
        return true;
    }
    return IsDialogMessageW(window_, &msg) != FALSE;
}
void CardEditor::show_information(const application::CardSummary& card) {
    if (window_)
        DestroyWindow(window_);
    open(card, Mode::information);
}
static std::vector<PopupItem> card_rows(const application::CardMenuSnapshot& card, const EditorActions& actions) {
    std::vector<PopupItem> rows;
    constexpr wchar_t glyphs[] = {fluent::Icon::font_size, fluent::Icon::bold, fluent::Icon::ink, fluent::Icon::appearance, fluent::Icon::opacity, fluent::Icon::edit,
                                  fluent::Icon::information, fluent::Icon::copy, fluent::Icon::folder, fluent::Icon::refresh, fluent::Icon::information, fluent::Icon::save_as};
    for (const auto& item : items(card.menu)) {
        if (card.unclassified_path && item.command != CardCommand::refresh && item.command != CardCommand::reveal &&
            item.command != CardCommand::information)
            continue;
        using enum CardCommand;
        PopupItem row{int(item.command), item.label, glyphs[UINT(item.command) - UINT(font)]};
        switch (item.command) {
        case font:
            row.kind = PopupKind::number;
            row.value = number(card.style.font_size);
            break;
        case bold:
            row.kind = PopupKind::toggle;
            row.checked = card.style.bold;
            row.commit_toggle = [&actions, entry = card.entry](bool checked) -> PopupResult {
                auto current = actions.current(entry);
                current.style.bold = checked;
                actions.save_style(entry.id, current.style, false);
                return {};
            };
            break;
        case opacity:
            row.kind = PopupKind::opacity;
            row.value = number(card.style.opacity * 100);
            break;
        case foreground:
        case background:
            row.kind = PopupKind::color;
            row.color = item.command == foreground ? card.style.text_color : card.style.background_color;
            row.color_default = item.command == foreground ? domain::ink_default : domain::paper_default;
            row.color_alpha = item.command == background;
            row.pick_color = [&actions, entry = card.entry, paper = item.command == background](HWND owner,
                                                                                                HWND anchor) {
                auto current = actions.current(entry);
                const auto original = paper ? current.style.background_color : current.style.text_color;
                ColorPickerOptions options;
                options.defer_history = actions.defer_history;
                options.value = original;
                options.default_color = paper ? domain::paper_default : domain::ink_default;
                options.allow_alpha = paper;
                options.large_text =
                    current.style.font_size >= 24 || (current.style.bold && current.style.font_size >= 18.67);
                options.translucent = current.style.opacity < 1;
                if (paper)
                    options.contrast_ink = current.style.ink_rgb();
                else
                    options.contrast_background = current.style.paper_rgb();
                options.callbacks.preview = [&actions, entry, paper](auto color) {
                    actions.preview_color(entry, paper, color);
                };
                options.callbacks.cancel = [&actions, entry, paper] {
                    actions.preview_color(entry, paper, {});
                };
                options.callbacks.valid = [&actions, entry, paper, original] {
                    try {
                        auto c = actions.current(entry);
                        return c.entry.revision == entry.revision &&
                               (paper ? c.style.background_color : c.style.text_color) == original;
                    } catch (...) {
                        return false;
                    }
                };
                options.callbacks.commit = [&actions, entry, paper, original](auto color) {
                    actions.save_color(entry, paper, original, color);
                    actions.preview_color(entry, paper, {});
                    return presentation::ColorCommit::saved;
                };
                ColorPickerPopup::track(owner, anchor, std::move(options));
                auto saved = actions.current(entry);
                return paper ? saved.style.background_color : saved.style.text_color;
            };
            break;
        case edit:
            row.hint = L"Enter";
            row.preferred = true;
            break;
        case information:
            row.literal = true;
            row.chevron = true;
            break;
        default:
            break;
        }
        if (row.kind != PopupKind::command && row.kind != PopupKind::toggle && row.kind != PopupKind::color) {
            row.commit = [&actions, entry = card.entry, cmd = item.command](const std::wstring& input) -> PopupResult {
                auto current = actions.current(entry);
                auto& style = current.style;
                if (cmd == font || cmd == opacity) {
                    auto n = decimal(ascii(input), cmd == font ? 6 : 0, cmd == font ? 96 : 100);
                    if (!n)
                        return {cmd == font ? tr(TextKey::error_card_size)
                                            : tr(TextKey::error_opacity)};
                    if (cmd == font)
                        style.font_size = *n;
                    else
                        style.opacity = *n / 100.0;
                }
                actions.save_style(entry.id, style, cmd == font);
                return {};
            };
        }
        row.divider = item.command == edit;
        rows.push_back(std::move(row));
    }
    return rows;
}
static UiText card_subtitle(const application::CardMenuSnapshot& card) {
    const UiText kind = card.unclassified_path ? tr(TextKey::card_kind_reference) : card_type(card.menu);
    if (!card.entry.rope_id)
        return message(TextKey::card_subtitle_floating, {{"kind", kind}});
    if (const auto number = card.position)
        return message(TextKey::card_subtitle_hanging, {{"kind", kind}, {"index", std::to_wstring(number->first)},
                                                       {"count", std::to_wstring(number->second)}});
    return kind;
}
void CardEditor::menu(HWND owner, POINT screen, application::CardMenuSnapshot card) {
    PopupMenuHost::RefreshCommands refresh;
    if (card.unclassified_path) {
        refresh = [&](UiText& subtitle, std::vector<PopupItem>& rows) {
            const auto kind = actions_.confirm_path_kind(card);
            if (kind == domain::PathKind::unknown) return false;
            card.unclassified_path = false;
            card.menu = kind == domain::PathKind::directory ? application::CardMenuKind::folder :
                                                             application::CardMenuKind::single_file;
            subtitle = card_subtitle(card);
            rows = card_rows(card, actions_);
            return true;
        };
    }
    const int choice = PopupMenuHost::track(owner, screen, tr(TextKey::card_settings), card_subtitle(card),
        card_rows(card, actions_), PopupFace::card, std::move(refresh), nullptr, nullptr, actions_.defer_history);
    if (!choice)
        return;
    const auto command = static_cast<CardCommand>(choice);
    if (command == CardCommand::expand && card.menu == application::CardMenuKind::multiple_files) {
        actions_.expand(card.entry, owner);
        return;
    }
    auto current = actions_.current(card.entry);
    const bool common_file_action = card.unclassified_path &&
        (command == CardCommand::refresh || command == CardCommand::reveal) &&
        (current.menu == application::CardMenuKind::folder || current.menu == application::CardMenuKind::single_file);
    if (current.menu != card.menu && !common_file_action && command != CardCommand::information)
        return;
    switch (command) {
    case CardCommand::edit:
        if (current.entry.rope_id == card.entry.rope_id && current.entry.revision == card.entry.revision)
            actions_.edit_text(current.entry);
        return;
    case CardCommand::open:
    case CardCommand::reveal:
    case CardCommand::refresh:
        actions_.file(command, current, owner);
        return;
    default:
        break;
    }
    if (window_)
        DestroyWindow(window_);
    open(current, Mode::information);
}
void CardEditor::open(const application::CardSummary& card, Mode mode) {
    entry_ = card.entry;
    mode_ = mode;
    showing_information_ = true;
    try {
        details_ = actions_.details(card);
    } catch (const application::StaleEdit&) {
        details_ = {{application::DetailLine::note, {}, {}, application::DetailText::card_gone}};
        showing_information_ = false;
    } catch (const std::exception&) {
        details_ = {{application::DetailLine::note, {}, {}, application::DetailText::information_unavailable}};
    }
    show();
}

void CardEditor::file_notice(HWND owner, UiText reason, std::wstring path) {
    CardEditor notice({});
    notice.mode_ = Mode::notice;
    notice.notice_reason_ = std::move(reason);
    notice.details_ = {{application::DetailLine::note, {}, {}},
                       {application::DetailLine::field, {}, std::move(path), application::DetailText::path}};
    notice.show(owner);
    if (!notice.window_) return;
    const bool disable = IsWindowEnabled(owner);
    if (disable) EnableWindow(owner, FALSE);
    MSG msg{};
    while (notice.window_) {
        int result = GetMessageW(&msg, nullptr, 0, 0);
        if (result <= 0) {
            if (!result) PostQuitMessage(int(msg.wParam));
            break;
        }
        if (!notice.dialog_message(msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    }
    if (disable && IsWindow(owner)) EnableWindow(owner, TRUE);
}

void CardEditor::show(HWND owner) {
    if (!rich_edit_) rich_edit_ = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!rich_edit_) return;
    WNDCLASSW wc{};
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    wc.lpszClassName = editor_class;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;
    constexpr DWORD ex = WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST;
    constexpr DWORD flags = WS_OVERLAPPED | WS_SYSMENU | WS_CAPTION | WS_THICKFRAME | WS_CLIPCHILDREN;
    const auto title = mode_ == Mode::notice ? tr(TextKey::card_file_information) : tr(TextKey::card_information);
    window_ = create_ui_window(ex, editor_class, title, flags, CW_USEDEFAULT, CW_USEDEFAULT, 550, 350, owner,
                              nullptr, wc.hInstance, this);
    if (!window_)
        return;
    const auto dpi = fluent::design_dpi(GetDpiForWindow(window_));
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT rect{0, 0, std::min<LONG>(MulDiv(514, dpi, 96), monitor.rcWork.right - monitor.rcWork.left),
                    std::min<LONG>(MulDiv(460, dpi, 96), monitor.rcWork.bottom - monitor.rcWork.top)};
    // Measure the wrapped document at its final width before showing the dialog.
    SetWindowPos(window_, nullptr, 0, 0, rect.right, rect.bottom, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    SendMessageW(GetDlgItem(window_, content), EM_REQUESTRESIZE, 0, 0);
    const int height = std::clamp(133 + fluent::dip(window_, text_height_), 216, 460);
    rect.bottom = std::min<LONG>(MulDiv(height, dpi, 96), monitor.rcWork.bottom - monitor.rcWork.top);
    SetWindowPos(window_, HWND_TOPMOST, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                 SWP_NOMOVE | SWP_SHOWWINDOW);
    SetForegroundWindow(window_);
    SetFocus(GetDlgItem(window_, fluent::dialog_close_id));
}
void CardEditor::refresh_information() {
    if (!window_ || !showing_information_ || mode_ != Mode::information)
        return;
    application::CardDetails updated;
    try {
        const auto card = actions_.current(entry_);
        updated = actions_.details(card);
    } catch (const std::exception&) {
        updated = {{application::DetailLine::note, {}, {}, application::DetailText::card_gone}};
        showing_information_ = false;
    }
    if (updated != details_) {
        details_ = std::move(updated);
        format_details();
    }
}
bool CardEditor::controls() {
    if (!theme_.dialog_chrome(window_)) return false;
    auto title = fluent::child(window_, L"STATIC", tr(mode_ == Mode::notice ? TextKey::card_file_information : TextKey::card_information), SS_NOPREFIX, heading);
    auto body = fluent::child(window_, MSFTEDIT_CLASS, L"",
                              WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, content);
    if (!title || !body || !scrollbar_.create(window_, window_, theme_.colors, true,
            [this](int position) { scroll_to(position); }) ||
        !SetWindowSubclass(body, details_proc, 1, DWORD_PTR(this))) return false;
    SendMessageW(body, EM_SETEVENTMASK, 0, SendMessageW(body, EM_GETEVENTMASK, 0, 0) | ENM_REQUESTRESIZE | ENM_SCROLL);
    theme_.font(title, fluent::Font::title);
    fluent::accessible_name(body, mode_ == Mode::notice ? tr(TextKey::card_file_details) : tr(TextKey::card_details));
    format_details();
    return true;
}

void CardEditor::format_details(bool refresh_theme) {
    using Line = application::DetailLine;
    auto body = GetDlgItem(window_, content);
    if (!body) return;
    formatting_ = true;
    const LONG anchor = first_visible_char(body);
    CHARRANGE selection{};
    POINT scroll{};
    SendMessageW(body, EM_EXGETSEL, 0, LPARAM(&selection));
    SendMessageW(body, EM_GETSCROLLPOS, 0, LPARAM(&scroll));
    SendMessageW(body, WM_SETREDRAW, FALSE, 0);
    if (refresh_theme) theme_.refresh();
    std::wstring text;
    std::vector<LONG> starts;
    std::vector<std::pair<std::wstring, std::wstring>> resolved;
    for (const auto& row : details_) {
        if (!text.empty()) text += L'\r';
        starts.push_back(LONG(text.size()));
        auto [label, value] = presentation::detail_labels(row, ui_localization());
        if (mode_ == Mode::notice && row.kind == Line::note) label = notice_reason_;
        text += label;
        if (!value.empty()) text += L"\t" + value;
        resolved.emplace_back(std::move(label), std::move(value));
    }
    set_ui_text(body, text);
    auto& p = theme_.colors;
    SendMessageW(body, EM_SETBKGNDCOLOR, 0, p.panel);
    const auto dpi = GetDpiForWindow(window_);
    SendMessageW(body, EM_SETZOOM, fluent::design_dpi(dpi), dpi);
    auto style = [&](LONG begin, LONG end, int size, COLORREF color, bool bold) {
        CHARRANGE range{begin, end};
        SendMessageW(body, EM_EXSETSEL, 0, LPARAM(&range));
        CHARFORMAT2W cf{sizeof(cf)};
        cf.dwMask = CFM_FACE | CFM_SIZE | CFM_COLOR | CFM_BOLD;
        cf.dwEffects = bold ? CFE_BOLD : 0;
        cf.yHeight = size * 15;
        cf.crTextColor = color;
        wcscpy_s(cf.szFaceName, ui_font_family());
        SendMessageW(body, EM_SETCHARFORMAT, SCF_SELECTION, LPARAM(&cf));
    };
    for (size_t i = 0; i < details_.size(); ++i) {
        const auto& row = details_[i];
        const LONG start = starts[i], end = i+1 < starts.size() ? starts[i+1]-1 : LONG(text.size());
        const bool heading_row = row.kind == Line::heading;
        style(start, end, heading_row ? 16 : 14, p.text, heading_row);
        PARAFORMAT2 pf{sizeof(pf)};
        pf.dwMask = PFM_SPACEBEFORE | PFM_SPACEAFTER | PFM_TABSTOPS | PFM_STARTINDENT | PFM_OFFSET;
        pf.dySpaceBefore = heading_row ? 140 : 0;
        pf.dySpaceAfter = 90;
        pf.cTabCount = 1;
        pf.rgxTabs[0] = (row.kind == Line::format ? 238 : 115) * 15;
        if (!resolved[i].second.empty()) pf.dxOffset = pf.rgxTabs[0];
        SendMessageW(body, EM_SETPARAFORMAT, 0, LPARAM(&pf));
        if (row.kind == Line::field) style(start, start + LONG(resolved[i].first.size()), 13, p.muted, false);
        if (row.kind == Line::format) style(start + LONG(resolved[i].first.size())+1, end, 13, p.muted, false);
    }
    SendMessageW(body, EM_EXSETSEL, 0, LPARAM(&selection));
    SendMessageW(body, WM_SETREDRAW, TRUE, 0);
    SendMessageW(body, EM_REQUESTRESIZE, 0, 0);
    SendMessageW(body, EM_SETSCROLLPOS, 0, LPARAM(&scroll));
    restore_visible_line(body, anchor);
    InvalidateRect(body, nullptr, TRUE);
    formatting_ = false;
    sync_scroll();
}
void CardEditor::sync_scroll() {
    if (formatting_) return;
    HWND body = GetDlgItem(window_, content);
    if (!body) return;
    RECT client{};
    GetClientRect(body, &client);
    POINT pos{};
    SendMessageW(body, EM_GETSCROLLPOS, 0, LPARAM(&pos));
    scrollbar_.update(pos.y, std::max<LONG>(pos.y, text_height_ - client.bottom),
                      client.bottom, fluent::px(window_, 16));
}
void CardEditor::scroll_to(int position) {
    auto body = GetDlgItem(window_, content);
    POINT pos{0, position};
    SendMessageW(body, EM_SETSCROLLPOS, 0, LPARAM(&pos));
    sync_scroll();
}
LRESULT CALLBACK CardEditor::details_proc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<CardEditor*>(data);
    if (msg == WM_MOUSEWHEEL) {
        self->scrollbar_.wheel(w);
        return 0;
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(h, details_proc, id);
    const auto result = DefSubclassProc(h, msg, w, l);
    if (msg == WM_PAINT || msg == WM_VSCROLL || msg == WM_KEYDOWN || msg == EM_LINESCROLL || msg == EM_SETSCROLLPOS)
        self->sync_scroll();
    return result;
}
void CardEditor::layout() {
    RECT r{};
    GetClientRect(window_, &r);
    int w = fluent::dip(window_, r.right), h = fluent::dip(window_, r.bottom);
    fluent::place(GetDlgItem(window_, heading), 25, 20, w - 82, 35);
    HWND body = GetDlgItem(window_, content);
    const LONG anchor = first_visible_char(body);
    fluent::place(body, 41, 84, w - 82, h - 125);
    fluent::place(scrollbar_.window(), w - 41, 84, 15, h - 125);
    SendMessageW(body, EM_REQUESTRESIZE, 0, 0);
    restore_visible_line(body, anchor);
    sync_scroll();
}
LRESULT CALLBACK CardEditor::proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<CardEditor*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<CardEditor*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->window_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(h, msg, w, l);
    switch (msg) {
    case WM_CREATE: {
        if (!self->controls())
            return -1;
        return 0;
    }
    case WM_MOUSEWHEEL:
        self->scrollbar_.wheel(w);
        return 0;
    case WM_NOTIFY: {
        auto header = reinterpret_cast<NMHDR*>(l);
        if (header->idFrom == content && header->code == EN_REQUESTRESIZE) {
            const RECT bounds = reinterpret_cast<REQRESIZE*>(l)->rc;
            self->text_height_ = bounds.bottom - bounds.top;
            self->sync_scroll();
            return 0;
        }
        break;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return self->theme_.control_color(HDC(w), HWND(l), GetDlgCtrlID(HWND(l)) == content);
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        auto dc = BeginPaint(h, &ps);
        RECT r{};
        GetClientRect(h, &r);
        fluent::solid(dc, r, self->theme_.colors.window);
        InflateRect(&r, -fluent::px(h, 23), -fluent::px(h, 23));
        r.top = fluent::px(h, 67);
        self->theme_.draw_card(h, dc, r, {fluent::CardPart::panel});
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(h, &r);
        FillRect(HDC(w), &r, self->theme_.background());
        return 1;
    }
    case language_changed:
        self->format_details();
        self->layout();
        return 0;
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        self->format_details(true);
        return 0;
    case WM_SIZE:
        self->layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(l);
        auto dpi = GetDpiForWindow(h);
        mm->ptMinTrackSize = {MulDiv(400, dpi, 96), MulDiv(210, dpi, 96)};
        return 0;
    }
    case WM_DPICHANGED: {
        self->format_details(true);
        auto r = reinterpret_cast<const RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case DM_GETDEFID:
        return MAKELRESULT(fluent::dialog_close_id, DC_HASDEFID);
    case WM_COMMAND:
        if (LOWORD(w) == content && HIWORD(w) == EN_VSCROLL) {
            self->sync_scroll();
            return 0;
        }
        if (LOWORD(w) == IDOK) {
            DestroyWindow(h);
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) {
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        self->window_ = nullptr;
        self->entry_ = {};
        self->details_.clear();
        self->theme_.release();
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}
} // namespace cliprope::win
