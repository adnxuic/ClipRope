#include "platform_win/inline_editor_host.hpp"
#include "presentation/card_labels.hpp"
#include "rendering/workspace_paint.hpp"
#include "presentation/card_capabilities.hpp"
#include <richedit.h>
#include <richole.h>
#include <tom.h>
#include <wrl.h>
#include <commctrl.h>
#include <imm.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace cliprope::win {
namespace {
constexpr wchar_t host_class[] = L"ClipRope.InlineEditorHost";
constexpr int text_field = 1760, edit_notice = 1761;
constexpr ULONGLONG presentation_retry_ms = 500;
COLORREF color(unsigned rgb) { return RGB(rgb >> 16, rgb >> 8, rgb); }
int pixels(int dip, UINT dpi) { return MulDiv(dip, dpi, 96); }

bool same_paragraphs(std::wstring_view left, std::wstring_view right) {
    auto next = [](std::wstring_view& s) {
        const auto ch = s.front();
        s.remove_prefix(1);
        if (ch != L'\r') return ch;
        if (s.starts_with(L'\n')) s.remove_prefix(1);
        return L'\n';
    };
    while (!left.empty() && !right.empty())
        if (next(left) != next(right)) return false;
    return left.empty() && right.empty();
}
bool composing(HWND field) {
    auto imc = ImmGetContext(field);
    if (!imc) return false;
    const auto length = ImmGetCompositionStringW(imc, GCS_COMPSTR, nullptr, 0);
    ImmReleaseContext(field, imc);
    return length > 0;
}
}

InlineEditorHost::~InlineEditorHost() {
    drain();
    if (host_) DestroyWindow(host_);
    if (paper_) DeleteObject(paper_);
    if (notice_font_) DeleteObject(notice_font_);
    if (library_) FreeLibrary(library_);
}

void InlineEditorHost::create() {
    if (host_) return;
    if (!library_) library_ = LoadLibraryExW(L"Msftedit.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!library_) throw std::runtime_error("Rich Edit is unavailable");
    WNDCLASSW cls{};
    cls.lpfnWndProc = proc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.lpszClassName = host_class;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Could not register the inline editor");
    host_ = create_ui_window(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, host_class, tr(TextKey::card_editor_title),
        WS_POPUP | WS_CLIPCHILDREN, 0, 0, 1, 1, nullptr, nullptr, cls.hInstance, this);
    if (!host_) throw std::runtime_error("Could not create the inline editor");
    surface_.attach(host_);
    auto child = [&](const wchar_t* cls_name, UiText label, DWORD style, int id) {
        return create_ui_window(0, cls_name, label, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1, host_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), cls.hInstance, nullptr);
    };
    auto action = [&](int id, fluent::Icon icon, UiText name) {
        auto h = fluent::child(host_,L"BUTTON",name,WS_TABSTOP|BS_PUSHBUTTON,id);
        if (h) {
            fluent::accessible_name(h,name);
            surface_.paper_style(h,icon,[this] { return fluent::PaperInk{paper_,background_,ink_}; });
        }
        return h;
    };
    field_ = child(MSFTEDIT_CLASS, L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN, text_field);
    if (!field_ || !SetWindowSubclass(field_, field_proc, 1, reinterpret_cast<DWORD_PTR>(this)) || !action(IDOK,fluent::Icon::check,tr(TextKey::common_save)) ||
        !action(IDCANCEL,fluent::Icon::close,tr(TextKey::common_cancel)) ||
        !child(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, edit_notice) ||
        SendMessageW(field_, EM_SETTEXTMODE, TM_PLAINTEXT | TM_MULTILEVELUNDO, 0)) {
        DestroyWindow(host_);
        host_ = field_ = nullptr;
        throw std::runtime_error("Could not initialize the inline controls");
    }
    SendMessageW(field_, EM_EXLIMITTEXT, 0, application::max_edit_units);
}

void InlineEditorHost::bind(HWND owner, application::TextDraft draft, const application::CardSummary& card,
                            Save write, std::function<bool()> present, Ended ended) {
    if (owner_) throw std::logic_error("An inline editor is already bound");
    create();
    SetWindowLongPtrW(host_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    owner_ = owner;
    draft_ = std::make_shared<application::TextDraft>(std::move(draft));
    entry_ = card.entry;
    const auto& style = card.style;
    style_ = card.style;
    write_ = std::move(write);
    present_ = std::move(present);
    ended_ = std::move(ended);
    try {
        paper_dpi_ = 0;
        background_ = color(rendering::paper_color(style.background_color));
        ink_ = color(style.ink_rgb());
        if (paper_) DeleteObject(paper_);
        paper_ = CreateSolidBrush(background_);
        if (!paper_ || !SetLayeredWindowAttributes(host_, 0, static_cast<BYTE>(style.opacity == 0 ? 255 :
                std::clamp(std::lround(style.opacity * 255), 1L, 255L)), LWA_ALPHA))
            throw std::runtime_error("Could not prepare the editor paper");
        SendMessageW(field_, EM_SETBKGNDCOLOR, 0, background_);
        if (!set_ui_text(field_, draft_->text)) throw std::runtime_error("Could not load the text");
        format();
        SendMessageW(field_, EM_SETSEL, 0, 0);
        SendMessageW(field_, EM_EMPTYUNDOBUFFER, 0, 0);
        notice(draft_->rich ? tr(TextKey::card_editor_plain) : L"");
        if (binding_changed) binding_changed();
    } catch (...) { discard(); throw; }
}
void InlineEditorHost::font_size(double size) {
    if (!owner_ || style_.font_size == size) return;
    style_.font_size = size;
    if (preview_) preview_->style.font_size = size;
    format();
}
void InlineEditorHost::colors(const application::CardStyle &style) {
    if (!field_ || !owner_)
        return;
    if (style_.text_color == style.text_color && style_.background_color == style.background_color)
        return;
    style_.text_color = style.text_color;
    style_.background_color = style.background_color;
    if (preview_) {
        preview_->style.text_color = style.text_color;
        preview_->style.background_color = style.background_color;
    }
    ink_ = color(style.ink_rgb());
    background_ = color(style.paper_rgb());
    if (paper_)
        DeleteObject(paper_);
    paper_ = CreateSolidBrush(background_);
    SendMessageW(field_, EM_SETBKGNDCOLOR, 0, background_);
    Microsoft::WRL::ComPtr<IRichEditOle> edit;
    Microsoft::WRL::ComPtr<ITextDocument> document;
    SendMessageW(field_, EM_GETOLEINTERFACE, 0, LPARAM(edit.GetAddressOf()));
    if (edit)
        edit.As(&document);
    const auto modified = SendMessageW(field_, EM_GETMODIFY, 0, 0);
    if (document)
        document->Undo(tomSuspend, nullptr);
    format();
    if (document)
        document->Undo(tomResume, nullptr);
    SendMessageW(field_, EM_SETMODIFY, modified, 0);
    paper_dpi_ = 0;
    if (!IsRectEmpty(&screen_) && !IsRectEmpty(&visible_))
        place(screen_, visible_, !entry_.rope_id);
    InvalidateRect(field_, nullptr, FALSE);
}
void InlineEditorHost::format() {
    CHARFORMAT2W fmt{};
    fmt.cbSize = sizeof fmt;
    fmt.dwMask = CFM_FACE | CFM_SIZE | CFM_BOLD | CFM_COLOR;
    fmt.dwEffects = style_.bold ? CFE_BOLD : 0;
    fmt.yHeight = static_cast<LONG>(std::lround(style_.font_size * 15));
    fmt.crTextColor = ink_;
    wcscpy_s(fmt.szFaceName, L"Segoe UI");
    SendMessageW(field_, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&fmt));
}
void InlineEditorHost::notice(UiText text) {
    set_ui_item_text(host_, edit_notice, text);
    layout();
    InvalidateRect(host_, nullptr, TRUE);
}
void InlineEditorHost::update_notice_font(UINT dpi) {
    auto next = CreateFontW(-pixels(10, dpi), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, ui_font_family());
    if (!next) return;
    SendDlgItemMessageW(host_, edit_notice, WM_SETFONT, reinterpret_cast<WPARAM>(next), TRUE);
    if (notice_font_) DeleteObject(notice_font_);
    notice_font_ = next;
}
void InlineEditorHost::layout() {
    if (!field_) return;
    RECT r{};
    GetClientRect(host_, &r);
    const auto dpi = GetDpiForWindow(host_);
    // The composited window border covers a caret at the field's first pixel.
    const auto margins = MAKELPARAM(pixels(2, dpi), 0);
    if (SendMessageW(field_, EM_GETMARGINS, 0, 0) != margins)
        SendMessageW(field_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, margins);
    const int size = std::min<LONG>(pixels(20, dpi), r.bottom / 2);
    const int top = r.bottom - size;
    const auto label = GetDlgItem(host_, edit_notice);
    const auto bound_message = ui_caption(label);
    const auto message = bound_message.view();
    const int count = int(message.size());
    const int beside = std::max(0L, r.right - size * 2);
    HDC dc = GetDC(label);
    auto old = SelectObject(dc, notice_font_ ? notice_font_ : GetStockObject(DEFAULT_GUI_FONT));
    SIZE line{};
    GetTextExtentPoint32W(dc, message.data(), count, &line);
    const bool stacked = count && line.cx > beside;
    RECT measure{0, 0, r.right, 0};
    if (stacked) DrawTextW(dc, message.data(), count, &measure, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    SelectObject(dc, old);
    ReleaseDC(label, dc);
    const int extra = stacked ? std::clamp<LONG>(measure.bottom, 0, std::max(0, top - pixels(12, dpi))) : 0;
    MoveWindow(field_, 0, 0, r.right, top - extra, TRUE);
    MoveWindow(label, 0, top - extra, extra ? r.right : beside, extra ? extra : size, TRUE);
    MoveWindow(GetDlgItem(host_, IDOK), r.right - 2 * size, top, size, size, TRUE);
    MoveWindow(GetDlgItem(host_, IDCANCEL), r.right - size, top, size, size, TRUE);
}
void InlineEditorHost::place(RECT screen, RECT visible, bool floating) {
    if (!owner_ || previewing() || IsRectEmpty(&visible)) return;
    const auto dpi = GetDpiForWindow(owner_);
    const int width = screen.right - screen.left;
    if (dpi != paper_dpi_ || width != paper_width_) {
        if (dpi != paper_dpi_) update_notice_font(dpi);
        const float x = floating ? rendering::TextInset::side : -float(width) * 48 / dpi;
        const float y = floating ? rendering::TextInset::floating_top :
            presentation::Hanging::paper_top + rendering::TextInset::hanging_top;
        if (auto brush = rendering::WorkspacePaint::acquire()->editor_paper(owner_, dpi,
                style_.background_color, entry(), x, y, floating)) {
            DeleteObject(paper_);
            paper_ = brush;
        }
        paper_dpi_ = dpi;
        paper_width_ = width;
    }
    if (!EqualRect(&screen, &screen_) || !EqualRect(&visible, &visible_)) {
        RECT clip = visible;
        OffsetRect(&clip, -screen.left, -screen.top);
        HRGN region = CreateRectRgnIndirect(&clip);
        if (!region || !SetWindowRgn(host_, region, FALSE)) {
            if (region) DeleteObject(region);
            throw std::runtime_error("Could not clip the editor");
        }
        if (!SetWindowPos(host_, HWND_TOPMOST, screen.left, screen.top, screen.right - screen.left,
                          screen.bottom - screen.top, SWP_NOACTIVATE))
            throw std::runtime_error("Could not position the editor");
        screen_ = screen;
        visible_ = visible;
        layout();
    }
    if (!IsWindowVisible(host_)) {
        ShowWindow(host_, SW_SHOWNOACTIVATE);
        focus();
    }
}
RECT InlineEditorHost::inset(HWND owner) const {
    RECT r{};
    if (!previewing() && owner_ && owner_ == owner && IsWindowVisible(host_)) {
        r = visible_;
        MapWindowPoints(nullptr, owner, reinterpret_cast<POINT*>(&r), 2);
    }
    return r;
}
void InlineEditorHost::focus() {
    if (owner_ && IsWindowVisible(host_)) {
        SetForegroundWindow(host_);
        SetFocus(field_);
    }
}
void InlineEditorHost::cancel() {
    if (!owner_ || previewing()) return;
    if (!SetTimer(host_, 1, 16, nullptr)) { notice(tr(TextKey::error_editor_close)); return; }
    phase_ = Phase::present_cancel;
    presentation_deadline_ = GetTickCount64() + presentation_retry_ms;
    handoff();
}
void InlineEditorHost::discard() {
    after_ = {};
    if (owner_) finish(nullptr);
}
void InlineEditorHost::finish(const application::CardSummary* saved) {
    auto ended = std::move(ended_);
    auto after = std::move(after_);
    owner_ = nullptr;
    phase_ = Phase::editing;
    composing_ = false;
    wheel_remainder_ = 0;
    ShowWindow(host_, SW_HIDE);
    if (ended) ended(saved);
    preview_.reset();
    submitted_.clear();
    draft_ = {};
    entry_ = {};
    write_ = {};
    present_ = {};
    screen_ = visible_ = {};
    SetWindowLongPtrW(host_, GWLP_HWNDPARENT, 0);
    SendMessageW(field_, EM_SETREADONLY, FALSE, 0);
    set_ui_text(field_, L"");
    SendMessageW(field_, EM_EMPTYUNDOBUFFER, 0, 0);
    if (binding_changed) binding_changed();
    for (auto& action : after) {
        try { action(); } catch (const std::exception&) { if (command_failed) command_failed(); }
    }
}
void InlineEditorHost::finish_then(std::function<void()> action) {
    if (!owner_) { action(); return; }
    after_.push_back(std::move(action));
    save();
}
void InlineEditorHost::save(std::function<void()> after) {
    if (!owner_) { if (after) after(); return; }
    if (phase_ == Phase::present_cancel) return;
    if (after && after_.empty()) after_.push_back(std::move(after));
    if (previewing()) return;
    try {
        if (composing_ || composing(field_)) {
            auto imc = ImmGetContext(field_);
            if (imc) {
                ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_COMPLETE, 0);
                ImmReleaseContext(field_, imc);
            }
            if (composing_ || composing(field_)) {
                after_ = {};
                notice(tr(TextKey::card_editor_finish_input));
                focus();
                return;
            }
        }
        GETTEXTLENGTHEX length{GTL_NUMCHARS | GTL_PRECISE | GTL_USECRLF, 1200};
        auto n = SendMessageW(field_, EM_GETTEXTLENGTHEX, reinterpret_cast<WPARAM>(&length), 0);
        if (n < 0) throw std::runtime_error("Could not size the text");
        std::wstring text(static_cast<size_t>(n) + 1, L'\0');
        GETTEXTEX get{static_cast<DWORD>(text.size() * sizeof(wchar_t)), GT_USECRLF, 1200};
        if (SendMessageW(field_, EM_GETTEXTEX, reinterpret_cast<WPARAM>(&get),
                         reinterpret_cast<LPARAM>(text.data())) != n)
            throw std::runtime_error("Could not read the complete text");
        text.resize(n);
        CHARRANGE selection{};
        SendMessageW(field_, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        selection_start_ = selection.cpMin;
        selection_end_ = selection.cpMax;
        SendMessageW(field_, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&saved_scroll_));
        application::CardSummary next;
        next.entry = entry_;
        next.style = style_;
        next.text = presentation::text_preview(text, next.paper_text);
        presentation::CardCapabilities formats(false);
        formats.retain(presentation::CardFormat::text);
        formats.apply(next);
        presentation::localize_card(next, ui_localization());
        next.menu = application::CardMenuKind::text;
        preview_ = std::move(next);
        submitted_ = std::move(text);
        if (!SetTimer(host_, 1, 16, nullptr)) throw std::runtime_error("Could not schedule edit completion");
        SendMessageW(field_, EM_SETREADONLY, TRUE, 0);
        phase_ = Phase::present_save;
        presentation_deadline_ = GetTickCount64() + presentation_retry_ms;
        handoff();
    } catch (const std::exception&) {
        restore(tr(TextKey::error_editor_save));
    }
}
void InlineEditorHost::handoff() {
    // The native inset still covers the owner's last (cut-out) frame.
    bool presented{};
    try { presented = present_(); }
    catch (const std::exception&) { /* A lost target can recover on the next timer tick. */ }
    if (!presented) {
        if (GetTickCount64() >= presentation_deadline_)
            restore(tr(phase_ == Phase::present_cancel ? TextKey::error_editor_close : TextKey::error_editor_save));
        return;
    }
    ShowWindow(host_, SW_HIDE);
    if (phase_ == Phase::present_cancel) { finish(nullptr); return; }
    phase_ = Phase::writing;
    try {
        auto launch = [this, draft = draft_, write = write_, text = std::make_shared<std::wstring>(std::move(submitted_))] {
            try {
                jobs_.push_back({draft, {}});
                jobs_.back().result = std::async(std::launch::async, [draft, write, text] {
                    if (same_paragraphs(*text, draft->text)) return domain::Id{};
                    return write(*draft, *text);
                });
            } catch (const std::exception&) {
                if (!jobs_.empty() && !jobs_.back().result.valid()) jobs_.pop_back();
                if (draft_ == draft) restore(tr(TextKey::error_editor_save));
            }
        };
        if (!defer_write || !defer_write(launch)) launch();
    } catch (const std::exception&) {
        if (!jobs_.empty() && !jobs_.back().result.valid()) jobs_.pop_back();
        restore(tr(TextKey::error_editor_save));
    }
}
void InlineEditorHost::restore(UiText message) {
    const bool restore_selection = phase_ != Phase::present_cancel;
    phase_ = Phase::editing;
    preview_.reset();
    after_ = {};
    SendMessageW(field_, EM_SETREADONLY, FALSE, 0);
    notice(message);
    ShowWindow(host_, SW_SHOWNOACTIVATE);
    focus();
    if (restore_selection) {
        CHARRANGE selection{selection_start_, selection_end_};
        SendMessageW(field_, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selection));
        SendMessageW(field_, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&saved_scroll_));
    }
    InvalidateRect(owner_, nullptr, FALSE);
    if (binding_changed) binding_changed();
}
void InlineEditorHost::complete(bool wait) {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (!wait && it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
        auto job = std::move(*it);
        it = jobs_.erase(it);
        const bool current = job.session == draft_ && phase_ == Phase::writing;
        domain::Id snapshot;
        try { snapshot = job.result.get(); }
        catch (const application::StaleEdit& stale) {
            if (current && stale.deleted) {
                discard();
                if (evicted) evicted(job.session->entry_id);
            } else if (current) restore(tr(TextKey::error_editor_changed));
            continue;
        }
        catch (const std::invalid_argument&) { if (current) restore(tr(TextKey::error_editor_text)); continue; }
        catch (const std::exception&) { if (current) restore(tr(TextKey::error_editor_save)); continue; }
        const bool changed = !snapshot.empty();
        if (current) {
            if (changed) {
                preview_->entry.snapshot_id = snapshot;
                ++preview_->entry.revision;
            }
            finish(changed ? &*preview_ : nullptr);
        }
        if (changed && committed) committed(*job.session, snapshot);
    }
    if (jobs_.empty() && phase_ == Phase::editing) KillTimer(host_, 1);
}
void InlineEditorHost::drain() {
    if (host_) complete(true);
}
void InlineEditorHost::wheel(WPARAM w) {
    if (previewing()) return;
    // With no visible WS_VSCROLL, Rich Edit may pass a wheel on to its
    // parent. Scroll its display lines explicitly and retain fractional turns.
    wheel_remainder_ += GET_WHEEL_DELTA_WPARAM(w);
    const int turns = wheel_remainder_ / WHEEL_DELTA;
    wheel_remainder_ %= WHEEL_DELTA;
    if (!turns) return;
    UINT lines = 3;
    SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
    if (lines == WHEEL_PAGESCROLL) {
        for (int i = 0; i < std::abs(turns); ++i)
            SendMessageW(field_, WM_VSCROLL, turns > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
    } else SendMessageW(field_, EM_LINESCROLL, 0, -turns * static_cast<int>(std::min(lines, 10000u)));
}
bool InlineEditorHost::translate(const MSG& m) {
    if (!owner_ || m.message != WM_KEYDOWN || !IsChild(host_, m.hwnd)) return false;
    const bool ctrl = GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_MENU) >= 0;
    if (m.wParam != VK_ESCAPE && m.wParam != VK_TAB && !(ctrl && m.wParam == 'S') &&
        !(m.wParam == VK_RETURN && m.hwnd != field_)) return false;
    if (composing_ || composing(field_)) return false;
    if (m.wParam == VK_TAB) {
        if (!ctrl) SetFocus(GetNextDlgTabItem(host_, m.hwnd, GetKeyState(VK_SHIFT) < 0));
    } else if (m.wParam == VK_ESCAPE || (m.wParam == VK_RETURN && GetDlgCtrlID(m.hwnd) == IDCANCEL)) cancel();
    else save();
    return true;
}
// Rich Edit draws selection, IME composition and glyphs natively. Composite its
// freshly painted pixels over the same paper tile as the Direct2D preview.
LRESULT CALLBACK InlineEditorHost::field_proc(HWND h, UINT msg, WPARAM w, LPARAM l, UINT_PTR, DWORD_PTR ref) {
    auto& self = *reinterpret_cast<InlineEditorHost*>(ref);
    if (msg == WM_MOUSEWHEEL) {
        self.wheel(w);
        return 0;
    }
    if (msg == WM_IME_STARTCOMPOSITION) self.composing_ = true;
    if (msg == WM_IME_ENDCOMPOSITION) self.composing_ = false;
    RECT r{};
    if (msg != WM_PAINT || !self.paper_ || !GetUpdateRect(h, &r, FALSE))
        return DefSubclassProc(h, msg, w, l);
    const BOOL caret = HideCaret(h);
    const auto result = DefSubclassProc(h, msg, w, l);
    HDC dc = GetDC(h);
    HDC source = CreateCompatibleDC(dc);
    const int width = r.right - r.left, height = r.bottom - r.top;
    auto bitmap = CreateCompatibleBitmap(dc, width, height);
    auto old = SelectObject(source, bitmap);
    if (old && BitBlt(source, 0, 0, width, height, dc, r.left, r.top, SRCCOPY)) {
        FillRect(dc, &r, self.paper_);
        TransparentBlt(dc, r.left, r.top, width, height, source, 0, 0, width, height, self.background_);
    }
    if (old) SelectObject(source, old);
    DeleteObject(bitmap);
    DeleteDC(source);
    ReleaseDC(h, dc);
    if (caret) ShowCaret(h);
    return result;
}
LRESULT CALLBACK InlineEditorHost::proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<InlineEditorHost*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<InlineEditorHost*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self) switch (msg) {
    case WM_TIMER:
        if (w == 1) {
            if (self->phase_ == Phase::present_save || self->phase_ == Phase::present_cancel) self->handoff();
            try { self->complete(false); }
            catch (const std::exception&) { if (self->command_failed) self->command_failed(); }
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        self->wheel(w);
        return 0;
    case WM_SIZE: self->layout(); return 0;
    case WM_DPICHANGED: self->surface_.refresh(); self->layout(); return 0;
    case WM_THEMECHANGED: case WM_SYSCOLORCHANGE: case WM_SETTINGCHANGE:
        self->surface_.refresh(); return 0;
    case WM_ERASEBKGND: {
        RECT r{}; GetClientRect(h, &r);
        if (self->paper_) FillRect(reinterpret_cast<HDC>(w), &r, self->paper_);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
        SetBkColor(reinterpret_cast<HDC>(w), self->background_);
        SetTextColor(reinterpret_cast<HDC>(w), self->ink_);
        return reinterpret_cast<LRESULT>(self->paper_);
    case language_changed:
        refresh_ui_bindings(h);
        if (self->preview_) presentation::localize_card(*self->preview_, ui_localization());
        if (self->owner_) {
            POINT scroll{};
            SendMessageW(self->field_, EM_GETSCROLLPOS, 0, LPARAM(&scroll));
            self->update_notice_font(GetDpiForWindow(h));
            self->layout();
            SendMessageW(self->field_, EM_SETSCROLLPOS, 0, LPARAM(&scroll));
        }
        RedrawWindow(h, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
        return 0;
    case WM_CLOSE:
        self->save();
        return 0;
    case WM_COMMAND:
        if (HIWORD(w) == BN_CLICKED && LOWORD(w) == IDOK) self->save();
        if (HIWORD(w) == BN_CLICKED && LOWORD(w) == IDCANCEL) self->cancel();
        return 0;
    }
    return DefWindowProcW(h, msg, w, l);
}
}
