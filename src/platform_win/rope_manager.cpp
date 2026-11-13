#include "platform_win/rope_manager.hpp"
#include "platform_win/popup_menu.hpp"
#include "platform_win/color_picker.hpp"
#include "platform_win/rope_name.hpp"
#include <charconv>
#include <cstdio>
#include <stdexcept>

namespace cliprope::win {
using application::RopeOp;
namespace {
enum Control : int { name = 1901, capacity, color, notice = 1905, name_label, cap_label, color_label };
using unicode::utf8;
std::string field(HWND window, int id) { return utf8(fluent::caption(GetDlgItem(window, id))); }
int value(std::string_view input, int base) {
    int out{};
    auto r = std::from_chars(input.data(), input.data() + input.size(), out, base);
    if (r.ec != std::errc{} || r.ptr != input.data() + input.size())
        throw std::invalid_argument("Invalid number");
    return out;
}
UiText title(RopeOp op) {
    switch (op) {
    case RopeOp::create:
        return tr(TextKey::rope_create);
    case RopeOp::properties:
        return tr(TextKey::rope_edit);
    case RopeOp::clear:
        return tr(TextKey::rope_clear);
    case RopeOp::erase:
        return tr(TextKey::rope_delete);
    default:
        break;
    }
    return L"ClipRope";
}
} // namespace
RopeManager::RopeManager(RopeActions actions) : actions_(std::move(actions)) {}
RopeManager::~RopeManager() {
    if (window_)
        DestroyWindow(window_);
}
bool RopeManager::dialog_message(MSG& m) const {
    return window_ && (window_ == m.hwnd || IsChild(window_, m.hwnd)) && IsDialogMessageW(window_, &m);
}
WorkspaceCommand RopeManager::menu(HWND owner, POINT screen, const domain::Rope& rope, WorkspaceMenuState state) {
    const auto multiple = state.multiple;
    constexpr int create = 105, properties = 106, clear = 108, erase = 110;
    auto shrink = std::make_shared<std::optional<application::RopePreview>>();
    std::vector<PopupItem> rows;
    rows.push_back({properties, tr(TextKey::rope_menu_edit), fluent::Icon::appearance});
    rows.back().detail = tr(TextKey::rope_menu_properties);
    for (auto id : {name, color, capacity}) {
        wchar_t rgb[8]{};
        swprintf_s(rgb, L"#%06X", rope.color.resolve(domain::rope_default).rgb());
        PopupItem item{int(id),
                       id == name    ? tr(TextKey::rope_name)
                       : id == color ? tr(TextKey::rope_color)
                                     : tr(TextKey::rope_capacity),
                       0,
                       id == name ? !rope.protected_rope() : id == color || rope.capacity.has_value(),
                       id == name    ? PopupKind::text
                       : id == color ? PopupKind::color
                                     : PopupKind::number,
                       id == name      ? unicode::wide(rope.name)
                       : id == color   ? rgb
                       : rope.capacity ? std::to_wstring(*rope.capacity)
                                       : std::wstring{}};
        if (id == color) {
            item.color = rope.color;
            item.color_default = domain::rope_default;
            item.pick_color = [this, rope_id = rope.id](HWND owner, HWND anchor) {
                const auto original = actions_.current(rope_id).color;
                ColorPickerOptions options;
                options.defer_history = actions_.defer_history;
                options.value = original;
                options.default_color = domain::rope_default;
                options.callbacks.preview = [this, rope_id](auto color) {
                    actions_.preview_color(rope_id, color);
                };
                options.callbacks.cancel = [this, rope_id] { actions_.preview_color(rope_id, {}); };
                options.callbacks.valid = [this, rope_id, original] {
                    try {
                        return actions_.current(rope_id).color == original;
                    } catch (...) {
                        return false;
                    }
                };
                options.callbacks.commit = [this, rope_id, original](auto color) {
                    actions_.save_color(rope_id, original, color);
                    actions_.preview_color(rope_id, {});
                    return presentation::ColorCommit::saved;
                };
                ColorPickerPopup::track(owner, anchor, std::move(options));
                return actions_.current(rope_id).color;
            };
        }
        if (id != color)
            item.commit = [this, rope_id = rope.id, shrink, id](const std::wstring &input) -> PopupResult {
                const auto current = actions_.current(rope_id);
                application::RopeChange change{RopeOp::properties, rope_id};
                change.name = current.name;
                change.capacity = current.capacity;
                change.color = current.color;
                try {
                    if (id == name)
                        change.name = edited_rope_name(input);
                    if (id == capacity)
                        change.capacity = value(utf8(input), 10);
                    auto preview = actions_.preview(change);
                    if (preview.shrink && window_)
                        return {tr(TextKey::error_rope_dialog_open)};
                    if (preview.shrink) {
                        *shrink = std::move(preview);
                        return {{}, true};
                    }
                    actions_.apply(preview);
                } catch (const application::InvalidRopeName&) {
                    return {tr(TextKey::error_rope_name)};
                } catch (const std::invalid_argument &) {
                    return {tr(TextKey::error_rope_values)};
                }
                return {};
            };
        rows.push_back(std::move(item));
    }
    auto command = [&](int id, UiText label, wchar_t icon, bool enabled = true) -> PopupItem& {
        rows.push_back({id, label, icon, enabled});
        return rows.back();
    };
    command(int(WorkspaceCommand::latest), tr(TextKey::rope_menu_first), fluent::Icon::first, state.has_cards).hint = tr(TextKey::rope_head);
    auto& density = command(int(WorkspaceCommand::density), tr(TextKey::rope_menu_density), fluent::Icon::density);
    density.kind = PopupKind::segments;
    density.segments = {tr(TextKey::rope_density_compact), tr(TextKey::rope_density_regular)};
    density.selection = state.compact ? 0 : 1;
    command(int(WorkspaceCommand::previous), tr(TextKey::rope_menu_previous), fluent::Icon::up, multiple).chevron = true;
    command(int(WorkspaceCommand::next), tr(TextKey::rope_menu_next), fluent::Icon::down, multiple).chevron = true;
    auto& bin = command(int(WorkspaceCommand::rebin), tr(TextKey::rope_menu_rebin), fluent::Icon::rebin);
    bin.kind = PopupKind::toggle;
    bin.checked = state.rebin_visible;
    auto& overview = command(int(WorkspaceCommand::overview), tr(TextKey::rope_menu_overview), fluent::Icon::overview);
    overview.kind = PopupKind::segments;
    overview.segments = {tr(TextKey::rope_overview_all), tr(TextKey::rope_overview_single)};
    overview.selection = state.overview ? 0 : 1;
    command(create, tr(TextKey::rope_menu_create), fluent::Icon::add).hint = tr(TextKey::rope_menu_create_hint);
    auto& empty = command(clear, tr(TextKey::rope_menu_clear), fluent::Icon::clear, state.has_cards);
    empty.chevron = true;
    empty.divider = true;
    command(int(WorkspaceCommand::find), tr(TextKey::rope_menu_find), fluent::Icon::search).hint = actions_.find_hint ? actions_.find_hint() : L"";
    auto& remove = command(erase, tr(TextKey::rope_menu_delete), fluent::Icon::erase, !rope.protected_rope());
    remove.divider = remove.danger = true;
    if (rope.protected_rope())
        remove.detail = rope.id == domain::rebin_id ? tr(TextKey::rope_rebin_protected) : tr(TextKey::rope_main_protected);
    const auto choice = PopupMenuHost::track(owner, screen, tr(TextKey::rope_settings), unicode::wide(rope.name), std::move(rows),
                                            PopupFace::rope, {}, nullptr, nullptr, actions_.defer_history);
    if (*shrink) {
        if (window_) {
            SetForegroundWindow(window_);
            return WorkspaceCommand::none;
        }
        auto proposed = actions_.current(rope.id);
        proposed.capacity = (*shrink)->change.capacity;
        open(owner, proposed, RopeOp::properties);
        if (window_)
            review(std::move(**shrink));
        return WorkspaceCommand::none;
    }
    switch (choice) {
    case create:
        open(owner, rope, RopeOp::create);
        break;
    case properties:
        open(owner, actions_.current(rope.id), RopeOp::properties);
        break;
    case clear:
        open(owner, actions_.current(rope.id), RopeOp::clear);
        break;
    case erase:
        if (!rope.protected_rope())
            open(owner, actions_.current(rope.id), RopeOp::erase);
        break;
    default:
        return static_cast<WorkspaceCommand>(choice);
    }
    return WorkspaceCommand::none;
}
void RopeManager::open(HWND owner, const domain::Rope& rope, RopeOp op) {
    if (op != RopeOp::create && op != RopeOp::properties && op != RopeOp::clear && op != RopeOp::erase)
        throw std::invalid_argument("Unsupported rope dialog");
    if (window_) {
        SetForegroundWindow(window_);
        return;
    }
    if (op == RopeOp::erase && rope.protected_rope())
        return;
    rope_ = rope;
    op_ = op;
    color_draft_ = op == RopeOp::create ? domain::ColorSelection{} : rope.color;
    consent_.reset();
    switching_ = false;
    if (op == RopeOp::create)
        rope_.capacity = actions_.default_capacity();
    UiText caption = title(op);
    if (op != RopeOp::create)
        caption += L" · " + unicode::wide(rope.name);
    show(owner, caption);
}
void RopeManager::confirm_switch(HWND owner, domain::Id created) {
    rope_.id = std::move(created);
    switching_ = true;
    show(owner, tr(TextKey::rope_switch_title));
}
void RopeManager::show(HWND owner, UiText caption) {
    WNDCLASSW wc{};
    wc.lpszClassName = L"ClipRope.RopeManager";
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = proc;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);

    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;
    constexpr DWORD ex = WS_EX_CONTROLPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
                    flags = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    HWND created = create_ui_window(ex, wc.lpszClassName, caption, flags, CW_USEDEFAULT, CW_USEDEFAULT, 460,
                                   290, owner, nullptr, wc.hInstance, this);
    if (!created)
        return;
    const auto dpi = fluent::design_dpi(GetDpiForWindow(created));
    RECT r{0, 0, MulDiv(460, dpi, 96),
           MulDiv(switching_ || op_ == RopeOp::erase || op_ == RopeOp::clear ? 198 : 310, dpi, 96)};
    SetWindowPos(created, HWND_TOPMOST, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_SHOWWINDOW);
    SetForegroundWindow(created);
    SetFocus(GetDlgItem(created, switching_ ? IDCANCEL :
                      op_ == RopeOp::create || op_ == RopeOp::properties ? name : IDOK));
}
bool RopeManager::controls() {
    auto add = [&](const wchar_t* cls, UiText label, DWORD styles, int id) {
        auto h = fluent::child(window_, cls, label, styles, id);
        if (!h)
            throw std::runtime_error("Could not create rope control");
        return h;
    };
    if (!theme_.dialog_chrome(window_)) return false;
    auto heading = add(L"STATIC", switching_ ? tr(TextKey::rope_switch_heading) : title(op_), SS_NOPREFIX, 1910);
    theme_.font(heading, fluent::Font::title);
    if (op_ != RopeOp::create || switching_) {
        auto target = add(L"STATIC", unicode::wide(rope_.name).c_str(), SS_NOPREFIX | SS_ENDELLIPSIS, 1911);
        theme_.font(target, fluent::Font::detail);
    }
    if (switching_) {
        add(L"STATIC", tr(TextKey::rope_switch_description), 0, notice);
        add(L"BUTTON", tr(TextKey::common_yes), WS_TABSTOP | BS_PUSHBUTTON, IDOK);
        add(L"BUTTON", tr(TextKey::common_no), WS_TABSTOP | BS_DEFPUSHBUTTON, IDCANCEL);
        return true;
    }
    const bool props = op_ == RopeOp::create || op_ == RopeOp::properties;
    if (props) {
        add(L"STATIC", tr(TextKey::rope_name_label), 0, name_label);
        auto edit = add(L"EDIT", op_ == RopeOp::create ? L"" : unicode::wide(rope_.name).c_str(),
                        WS_TABSTOP | ES_AUTOHSCROLL, name);
        if (op_ == RopeOp::properties && rope_.protected_rope())
            SendMessageW(edit, EM_SETREADONLY, TRUE, 0);
    }
    if (props) {
        add(L"STATIC", tr(TextKey::rope_capacity), 0, cap_label);
        const UiText label = rope_.capacity ? UiText(std::to_wstring(*rope_.capacity)) : tr(TextKey::rope_unlimited);
        add(L"EDIT", label, WS_TABSTOP | (rope_.capacity ? ES_NUMBER : ES_READONLY), capacity);
    }
    if (props) {
        wchar_t hex[8]{};
        swprintf_s(hex, L"#%06X", op_ == RopeOp::create ? domain::rope_default : rope_.color.resolve(domain::rope_default).rgb());
        add(L"STATIC", tr(TextKey::rope_color_label), 0, color_label);
        add(L"BUTTON", hex, WS_TABSTOP, color);
        color_picker_button(theme_, GetDlgItem(window_, color), color_draft_, domain::rope_default);
    }
    const auto text = op_ == RopeOp::create ? tr(TextKey::rope_create_note)
                      : op_ == RopeOp::properties
                          ? tr(TextKey::rope_shrink_note)
                          : tr(TextKey::rope_review_note);
    add(L"STATIC", text, 0, notice);
    add(L"BUTTON", props ? tr(TextKey::rope_save) : tr(TextKey::rope_preview), WS_TABSTOP | BS_DEFPUSHBUTTON, IDOK);
    add(L"BUTTON", tr(TextKey::rope_cancel), WS_TABSTOP | BS_PUSHBUTTON, IDCANCEL);
    return true;
}
void RopeManager::layout() {
    const auto dpi = fluent::design_dpi(GetDpiForWindow(window_));
    auto place = [&](int id, int x, int y, int w, int h) {
        if (auto widget = GetDlgItem(window_, id))
            MoveWindow(widget, MulDiv(x, dpi, 96), MulDiv(y, dpi, 96), MulDiv(w, dpi, 96), MulDiv(h, dpi, 96),
                       TRUE);
    };
    place(1910, 24, 14, 376, 34);
    place(1911, 25, 49, 410, 18);
    place(name_label, 24, 71, 136, 22);
    place(name, 164, 66, 270, 32);
    place(cap_label, 24, 116, 136, 22);
    place(capacity, 164, 110, 150, 32);
    place(color_label, 24, 161, 136, 22);
    place(color, 164, 154, 150, 36);
    const bool short_panel = switching_ || op_ == RopeOp::erase || op_ == RopeOp::clear;
    place(notice, 25, short_panel ? 76 : 201, 410, short_panel ? 61 : 50);
    int bottom = short_panel ? 146 : 256;
    place(IDOK, 224, bottom, 112, 34);
    place(IDCANCEL, 346, bottom, 88, 34);
}
void RopeManager::style_actions() {
    for (int id : {IDOK, IDCANCEL}) {
        HWND button = GetDlgItem(window_, id);
        auto label = ui_caption(button).display(true);
        const bool primary = switching_ ? id == IDCANCEL : id == IDOK;
        const bool danger = consent_ && id == IDOK;
        theme_.card_style(button, {fluent::CardPart::dialog_button, 0, std::move(label), {}, {}, COLORREF(danger), primary});
    }
}
void RopeManager::enable_inputs(bool enabled) {
    for (auto id : {name, capacity, color})
        if (auto h = GetDlgItem(window_, id))
            EnableWindow(h, enabled);
}
void RopeManager::review(application::RopePreview preview) {
    const auto impact = message(preview.hanging == 1 ? TextKey::rope_review_one : TextKey::rope_review_other,
                                {{"count", std::to_wstring(preview.hanging)}});
    set_ui_item_text(window_, notice, impact);
    if (preview.shrink)
        set_ui_item_text(window_, 1910, tr(TextKey::rope_shrink_confirm));
    set_ui_item_text(window_, IDOK,
                    preview.shrink         ? tr(TextKey::rope_confirm_shrink)
                    : op_ == RopeOp::erase ? tr(TextKey::rope_confirm_delete)
                                           : tr(TextKey::rope_confirm_clear));
    enable_inputs(false);
    consent_ = std::move(preview);
    style_actions();
}
void RopeManager::pick_color() {
    ColorPickerOptions options;
    options.value = color_draft_;
    options.default_color = domain::rope_default;
    options.callbacks.preview = [this](auto color) {
        if (op_ == RopeOp::properties)
            actions_.preview_color(rope_.id, color);
    };
    options.callbacks.cancel = [this] {
        if (op_ == RopeOp::properties)
            actions_.preview_color(rope_.id,
                                   color_draft_ == rope_.color ? std::nullopt : std::optional(color_draft_));
    };
    options.callbacks.valid = [this] {
        if (op_ != RopeOp::properties)
            return true;
        try {
            return actions_.current(rope_.id).color == rope_.color;
        } catch (...) {
            return false;
        }
    };
    options.callbacks.commit = [this](auto color) {
        color_draft_ = color;
        return presentation::ColorCommit::staged;
    };
    ColorPickerPopup::track(window_, GetDlgItem(window_, color), std::move(options));
    color_picker_button(theme_, GetDlgItem(window_, color), color_draft_, domain::rope_default);
}
void RopeManager::accept() {
    if (saving_.pending()) return;
    if (switching_) {
        const bool queued = saving_.submit(actions_.defer_history,
            [select = actions_.select, id = rope_.id] { select(id); },
            [this](auto error) {
                if (!error) { DestroyWindow(window_); return; }
                EnableWindow(GetDlgItem(window_, IDOK), TRUE);
                set_ui_item_text(window_, notice, tr(TextKey::error_rope_switch));
            }, true);
        if (queued) {
            EnableWindow(GetDlgItem(window_, IDOK), FALSE);
            set_ui_item_text(window_, notice, tr(TextKey::common_saving));
        }
        return;
    }
    try {
        application::RopeChange c = consent_ ? consent_->change : application::RopeChange{};
        c.op = op_;
        c.rope = rope_.id;
        if (!consent_ && (op_ == RopeOp::create || op_ == RopeOp::properties)) {
            c.name = edited_rope_name(fluent::caption(GetDlgItem(window_, name)));
            c.color = color_draft_;
        }
        if (!consent_ && (op_ == RopeOp::properties || op_ == RopeOp::create) && rope_.capacity)
            c.capacity = value(field(window_, capacity), 10);
        struct Result { std::optional<application::RopePreview> review; std::optional<domain::Id> created; };
        auto result = std::make_shared<Result>();
        const bool queued = saving_.submit(actions_.defer_history,
            [this, c, authorized = consent_, original = rope_.color, result] {
                if (authorized) { actions_.apply(*authorized); return; }
                if (c.op == RopeOp::properties && actions_.current(c.rope).color != original)
                    throw std::runtime_error("The rope color changed while properties were open");
                auto preview = actions_.preview(c);
                if (c.op == RopeOp::create || (c.op == RopeOp::properties && !preview.shrink))
                    result->created = actions_.apply(preview);
                else result->review = std::move(preview);
            }, [this, result, c](std::exception_ptr error) {
                EnableWindow(GetDlgItem(window_, IDOK), TRUE);
                if (error) {
                    consent_.reset();
                    enable_inputs(true);
                    set_ui_item_text(window_, IDOK, tr(TextKey::rope_review_again));
                    style_actions();
                    try { std::rethrow_exception(error); }
                    catch (const application::InvalidRopeName&) { set_ui_item_text(window_, notice, tr(TextKey::error_rope_name)); }
                    catch (const std::invalid_argument&) { set_ui_item_text(window_, notice, tr(TextKey::error_rope_fields)); }
                    catch (...) { set_ui_item_text(window_, notice, tr(TextKey::error_rope_commit)); }
                } else if (result->review) review(std::move(*result->review));
                else {
                    const auto owner = GetWindow(window_, GW_OWNER);
                    if (result->created) rope_.name = c.name;
                    DestroyWindow(window_);
                    if (result->created) confirm_switch(owner, *result->created);
                }
            }, true);
        if (queued) {
            EnableWindow(GetDlgItem(window_, IDOK), FALSE);
            enable_inputs(false);
            set_ui_item_text(window_, notice, tr(TextKey::common_saving));
        }
    } catch (const application::InvalidRopeName&) {
        set_ui_item_text(window_, notice, tr(TextKey::error_rope_name));
    } catch (const std::invalid_argument&) {
        set_ui_item_text(window_, notice,
                        tr(TextKey::error_rope_fields));
    } catch (...) {
        consent_.reset();
        enable_inputs(true);
        set_ui_item_text(window_, IDOK, tr(TextKey::rope_review_again));
        style_actions();
        set_ui_item_text(window_, notice,
                        tr(TextKey::error_rope_commit));
    }
}
LRESULT CALLBACK RopeManager::proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto* self = reinterpret_cast<RopeManager*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        self = static_cast<RopeManager*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        self->window_ = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self)
        return DefWindowProcW(h, msg, w, l);
    switch (msg) {
    case WM_CREATE: {
        try {
            if (!self->controls())
                return -1;
        } catch (...) {
            return -1;
        }
        for (auto ctrl = GetWindow(h, GW_CHILD); ctrl; ctrl = GetWindow(ctrl, GW_HWNDNEXT))
            if (GetDlgCtrlID(ctrl) != 1910 && GetDlgCtrlID(ctrl) != 1911)
                self->theme_.font(ctrl);
        for (auto id : {name, capacity})
            self->theme_.field(GetDlgItem(h, id));
        color_picker_button(self->theme_, GetDlgItem(h, color), self->color_draft_, domain::rope_default);
        self->style_actions();
        self->layout();
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
        return self->theme_.control_color(HDC(w), HWND(l),
                                          GetDlgCtrlID(HWND(l)) >= name && GetDlgCtrlID(HWND(l)) <= color);
    case WM_ERASEBKGND: {
        RECT r{};
        GetClientRect(h, &r);
        FillRect(HDC(w), &r, self->theme_.background());
        return 1;
    }
    case language_changed:
        color_picker_button(self->theme_, GetDlgItem(h, color), self->color_draft_, domain::rope_default);
        self->style_actions();
        self->layout();
        return 0;
    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        self->theme_.refresh();
        return 0;
    case WM_SIZE:
        self->layout();
        return 0;
    case WM_DPICHANGED: {
        self->theme_.refresh();
        auto r = reinterpret_cast<const RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER);
        return 0;
    }
    case DM_GETDEFID:
        return MAKELRESULT(self->switching_ ? IDCANCEL : IDOK, DC_HASDEFID);
    case WM_COMMAND:
        if (LOWORD(w) == color && HIWORD(w) == BN_CLICKED) {
            self->pick_color();
            return 0;
        }
        if (LOWORD(w) == IDOK) {
            self->accept();
            return 0;
        }
        if (LOWORD(w) == IDCANCEL) {
            DestroyWindow(h);
            return 0;
        }
        break;
    case WM_NCDESTROY:
        self->saving_.close();
        if (self->actions_.preview_color)
            self->actions_.preview_color(self->rope_.id, {});
        self->window_ = nullptr;
        self->consent_.reset();
        self->theme_.release();
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(h, msg, w, l);
}
} // namespace cliprope::win
