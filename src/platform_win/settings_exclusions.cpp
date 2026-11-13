#include "platform_win/settings.hpp"
#include "platform_win/file_basename.hpp"
#include "platform_win/unicode_name.hpp"
#include "platform_win/fluent_list.hpp"
#include <commctrl.h>
#include <span>

namespace cliprope::win {
namespace {
using namespace fluent;
enum Control { title = 1970, name_label, input, hint, list_label, list, remove_item, save };
const UiText heading = tr(TextKey::settings_exclusions);
const UiText guidance = tr(TextKey::error_executable_name);
struct ValidationError final : std::invalid_argument {
    TextKey key;
    explicit ValidationError(TextKey value) : std::invalid_argument("Invalid capture exclusion"), key(value) {}
};

void validate(std::wstring_view name, std::span<const std::wstring> previous) {
    if (name.size() > 255)
        throw ValidationError(TextKey::error_exclusions_long);
    if (name.size() <= 4 || !safe_file_basename(name) ||
        CompareStringOrdinal(name.data() + name.size() - 4, 4, L".exe", 4, TRUE) != CSTR_EQUAL ||
        name.find(wchar_t(127)) != name.npos)
        throw ValidationError(TextKey::error_exclusions_invalid);
    for (const auto& other : previous)
        if (CompareStringOrdinal(name.data(), int(name.size()), other.data(), int(other.size()), TRUE) == CSTR_EQUAL)
            throw ValidationError(TextKey::error_exclusions_duplicate);
}

struct Editor {
    SettingsSnapshot& settings;
    SettingsSnapshot::Exclusions original;
    Surface surface;
    HWND window{};
    bool failed{};
    PendingControlSave saving;

    HWND item(int id) const { return GetDlgItem(window, id); }

    std::vector<std::wstring> names() const {
        std::vector<std::wstring> result;
        const int count = int(SendDlgItemMessageW(window, list, LB_GETCOUNT, 0, 0));
        for (int i = 0; i < count; ++i) {
            const auto length = SendDlgItemMessageW(window, list, LB_GETTEXTLEN, i, 0);
            if (length == LB_ERR) throw std::runtime_error("Could not read the selected list.");
            std::wstring name(size_t(length) + 1, L'\0');
            if (SendDlgItemMessageW(window, list, LB_GETTEXT, i, LPARAM(name.data())) == LB_ERR)
                throw std::runtime_error("Could not read the selected list.");
            name.resize(size_t(length));
            result.push_back(std::move(name));
        }
        return result;
    }

    void append(const std::wstring& name) {
        if (SendDlgItemMessageW(window, list, LB_ADDSTRING, 0, LPARAM(name.c_str())) < 0)
            throw std::runtime_error("Could not add the executable name.");
    }

    void feedback(UiText text, bool error = false) {
        failed = error;
        set_ui_text(item(hint), text);
        accessible_description(item(input), text);
        if (error) NotifyWinEvent(EVENT_SYSTEM_ALERT, item(hint), OBJID_CLIENT, CHILDID_SELF);
    }

    void add_name() {
        auto name = caption(item(input));
        const auto existing = names();
        validate(name, existing);
        if (existing.size() >= 64) throw ValidationError(TextKey::error_exclusions_limit);
        append(name);
        SendDlgItemMessageW(window, list, LB_SETCURSEL, existing.size(), 0);
        EnableWindow(item(remove_item), TRUE);
        set_ui_text(item(input), L"");
        feedback(guidance);
        SetFocus(item(input));
    }

    void save_list() {
        if (GetWindowTextLengthW(item(input)))
            throw ValidationError(TextKey::error_exclusions_pending);
        const auto draft = names();
        SettingsSnapshot::Exclusions encoded;
        for (size_t i = 0; i < draft.size(); ++i) {
            try { validate(draft[i], std::span(draft).first(i)); }
            catch (const std::invalid_argument&) {
                SendDlgItemMessageW(window, list, LB_SETCURSEL, i, 0);
                EnableWindow(item(remove_item), TRUE);
                SetFocus(item(list));
                throw;
            }
            encoded.push_back(unicode::utf8(draft[i]));
        }
        const bool queued = saving.submit(settings.defer_history,
            [save = settings.save_exclusions, original = original, encoded = std::move(encoded)] {
                save(original, encoded);
            }, [this](auto error) {
                if (!error) { EndDialog(window, IDOK); return; }
                for (auto id : {input, list, save}) EnableWindow(item(id), TRUE);
                EnableWindow(item(IDOK), TRUE);
                EnableWindow(item(remove_item), SendDlgItemMessageW(window, list, LB_GETCURSEL, 0, 0) != LB_ERR);
                feedback(tr(TextKey::error_exclusions_save), true);
            }, true);
        if (queued) {
            feedback(tr(TextKey::common_saving));
            for (auto id : {input, list, remove_item, save}) EnableWindow(item(id), FALSE);
            EnableWindow(item(IDOK), FALSE);
        }
    }

    void layout() {
        RECT r{};
        GetClientRect(window, &r);
        const int width = dip(window, r.right), bottom = dip(window, r.bottom) - 52;
        place(item(title), 24, 14, width - 74, 36);
        place(item(name_label), 24, 65, width - 48, 21);
        place(item(input), 24, 89, width - 140, 34);
        place(item(IDOK), width - 108, 89, 84, 34);
        place(item(hint), 24, 129, width - 48, 32);
        place(item(list_label), 24, 169, width - 48, 22);
        place(item(list), 24, 193, width - 48, std::max(28, bottom - 207));
        place(item(remove_item), 24, bottom, 88, 34);
        place(item(save), width - 112, bottom, 88, 34);
    }

    void open() {
        set_ui_text(window, heading);
        if (!surface.dialog_chrome(window)) throw std::runtime_error("Could not create the list editor.");
        auto make = [&](int id, const wchar_t* type, UiText text, DWORD style = 0) {
            HWND h = child(window, type, text, style, id, id == input); // The native edit cue needs common-controls v6.
            if (!h) throw std::runtime_error("Could not create the list editor.");
            surface.font(h);
            return h;
        };
        surface.font(make(title, L"STATIC", heading), Font::title);
        make(name_label, L"STATIC", tr(TextKey::settings_exclusions_name));
        auto edit = make(input, L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL);
        surface.field(edit);
        accessible_name(edit, tr(TextKey::settings_exclusions_name));
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, LPARAM(ui_text(TextKey::settings_exclusions_example)));
        auto button = [&](int id, UiText text) {
            auto h = make(id, L"BUTTON", text, WS_TABSTOP | BS_NOTIFY | (id == IDOK ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON));
            surface.card_style(h, {.part = CardPart::dialog_button, .label = text, .selected = id == save});
        };
        button(IDOK, tr(TextKey::common_add));
        surface.font(make(hint, L"STATIC", guidance, SS_NOPREFIX), Font::detail);
        accessible_description(edit, guidance);
        make(list_label, L"STATIC", tr(TextKey::settings_exclusions_list));
        if (!list_box(surface,window,tr(TextKey::settings_exclusions_list),list))
            throw std::runtime_error("Could not create the exclusions list.");
        for (const auto& name : original) append(unicode::wide(name));
        button(remove_item, tr(TextKey::common_remove));
        EnableWindow(item(remove_item), FALSE);
        button(save, tr(TextKey::common_save));
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromWindow(GetWindow(window, GW_OWNER), MONITOR_DEFAULTTONEAREST), &monitor);
        const auto work = monitor.rcWork;
        const int width = std::min<LONG>(px(window, 500), work.right - work.left);
        const int height = std::min<LONG>(px(window, 390), work.bottom - work.top);
        SetWindowPos(window, nullptr, (work.left + work.right - width) / 2, (work.top + work.bottom - height) / 2,
                     width, height, SWP_SHOWWINDOW | SWP_NOZORDER);
        SetFocus(edit);
    }
};

INT_PTR CALLBACK dialog(HWND h, UINT msg, WPARAM w, LPARAM l) {
    auto editor = reinterpret_cast<Editor*>(GetWindowLongPtrW(h, DWLP_USER));
    if (msg == WM_INITDIALOG) {
        editor = reinterpret_cast<Editor*>(l);
        editor->window = h;
        SetWindowLongPtrW(h, DWLP_USER, l);
        try { editor->open(); }
        catch (const std::exception&) { EndDialog(h, -1); }
        return FALSE;
    }
    if (!editor) return FALSE;
    switch (msg) {
    case WM_SIZE: editor->layout(); return TRUE;
    case WM_DPICHANGED: {
        const auto& r = *reinterpret_cast<const RECT*>(l);
        SetWindowPos(h, nullptr, r.left, r.top, r.right-r.left, r.bottom-r.top, SWP_NOZORDER | SWP_NOACTIVATE);
        [[fallthrough]];
    }
    case language_changed:
        SendMessageW(editor->item(input), EM_SETCUEBANNER, TRUE, LPARAM(ui_text(TextKey::settings_exclusions_example)));
        editor->layout();
        return TRUE;
    case WM_THEMECHANGED: case WM_SYSCOLORCHANGE: case WM_SETTINGCHANGE:
        editor->surface.refresh();
        editor->layout();
        return TRUE;
    case WM_CTLCOLORDLG: return INT_PTR(editor->surface.background());
    case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX: return editor->surface.control_color(HDC(w), HWND(l), true);
    case WM_CTLCOLORSTATIC: {
        auto brush = editor->surface.control_color(HDC(w), HWND(l));
        if (HWND(l) == editor->item(hint) && editor->failed) SetTextColor(HDC(w), editor->surface.colors.danger);
        return brush;
    }
    case WM_CLOSE: editor->saving.close(); EndDialog(h, IDCANCEL); return TRUE;
    case WM_DESTROY: editor->saving.close(); return FALSE;
    case WM_COMMAND: {
        const int id = LOWORD(w), event = HIWORD(w);
        if (editor->saving.pending() && id != IDCANCEL) return TRUE;
        if (id == list && event == LBN_SELCHANGE) {
            EnableWindow(editor->item(remove_item), SendDlgItemMessageW(h, list, LB_GETCURSEL, 0, 0) != LB_ERR);
            return TRUE;
        }
        if ((id == input && event == EN_SETFOCUS) || (id == list && event == LBN_SETFOCUS) ||
            ((id == IDOK || id == save || id == remove_item) && event == BN_SETFOCUS)) {
            SendMessageW(h, DM_SETDEFID, id == input || id == list ? IDOK : id, 0);
            return TRUE;
        }
        if (event != BN_CLICKED) return FALSE;
        try {
            switch (id) {
            case IDCANCEL: editor->saving.close(); EndDialog(h, IDCANCEL); break;
            case IDOK: editor->add_name(); break;
            case remove_item: {
                const auto index = SendDlgItemMessageW(h, list, LB_GETCURSEL, 0, 0);
                if (index != LB_ERR) SendDlgItemMessageW(h, list, LB_DELETESTRING, index, 0);
                SendDlgItemMessageW(h, list, LB_SETCURSEL, WPARAM(-1), 0);
                EnableWindow(editor->item(remove_item), FALSE);
                SetFocus(editor->item(list));
                editor->feedback(guidance);
                break;
            }
            case save: editor->save_list(); break;
            default: return FALSE;
            }
        } catch (const ValidationError& e) { editor->feedback(tr(e.key), true); }
          catch (const std::exception&) { editor->feedback(tr(TextKey::error_exclusions_save), true); }
        return TRUE;
    }
    }
    return FALSE;
}
} // namespace

void Settings::edit_exclusions() {
    Editor editor{data_, data_.read_exclusions()};
    if (fluent::modal(window_, dialog, LPARAM(&editor)) == -1)
        throw std::runtime_error("Could not open the capture exclusions editor.");
}
} // namespace cliprope::win
