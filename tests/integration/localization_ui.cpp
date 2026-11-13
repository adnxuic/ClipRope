#include "history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "rendering/card_browser.hpp"
#include "platform_win/fluent.hpp"
#include "platform_win/popup_menu.hpp"
#include "presentation/card_labels.hpp"
#include "presentation/card_capabilities.hpp"
#include <cstring>
#include <iostream>
#include <ole2.h>

using namespace cliprope;
using namespace history_fixture;
namespace {
bool clicked{}, label_correct{};
void CALLBACK click_toggle(HWND owner, UINT, UINT_PTR timer, DWORD) {
    const auto popup = FindWindowW(L"ClipRope.CardMenu", nullptr);
    if (!popup) return;
    KillTimer(owner, timer);
    label_correct = win::fluent::caption(GetDlgItem(popup, 801)) == win::ui_localization().tr(localization::TextKey::card_bold);
    SendMessageW(popup, WM_COMMAND, MAKEWPARAM(801, BN_CLICKED), LPARAM(GetDlgItem(popup, 801)));
    clicked = true;
    PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
}
void toggle(HWND owner, bool fail) {
    bool saved{};
    clicked = label_correct = false;
    win::PopupItem item{801, win::tr(localization::TextKey::card_bold)};
    item.kind = win::PopupKind::toggle;
    item.checked = false;
    item.commit_toggle = [&](bool value) -> win::PopupResult {
        require(value, "Toggle did not submit a boolean true");
        if (fail) throw std::runtime_error("Injected style failure");
        saved = value;
        return {};
    };
    require(SetTimer(owner, 1, 10, click_toggle) != 0, "Could not schedule menu input");
    win::PopupMenuHost::track(owner, {40, 40}, win::tr(localization::TextKey::card_settings), L"On", {std::move(item)}, win::PopupFace::card);
    KillTimer(owner, 1);
    require(clicked && label_correct && saved == !fail, "Localized toggle changed meaning or failure committed");
}
localization::LocalizationManager* switching{};
HWND original_field{};
unsigned switch_step{};
bool live_preserved{};
void CALLBACK switch_open_menu(HWND owner, UINT, UINT_PTR timer, DWORD) {
    const auto popup = FindWindowW(L"ClipRope.PopupMenu", nullptr);
    if (!popup) return;
    const auto field = GetDlgItem(popup, 10830);
    if (switch_step++ == 0) {
        original_field = field;
        SendMessageW(field, EM_SETSEL, 1, 4);
        switching->set_language(localization::Language::simplified_chinese);
        win::notify_language_changed();
        return;
    }
    live_preserved = original_field == field && win::fluent::caption(field) == L"On Save" &&
        SendMessageW(field, EM_GETSEL, 0, 0) == MAKELRESULT(1, 4) &&
        win::fluent::caption(GetDlgItem(popup, 831)) == L"保存";
    KillTimer(owner, timer);
    PostMessageW(popup, WM_CLOSE, 0, 0);
}
void live_menu(HWND owner, localization::LocalizationManager& manager) {
    manager.set_language(localization::Language::english);
    switching = &manager;
    switch_step = 0;
    win::PopupItem name{830, win::tr(localization::TextKey::rope_name_label)};
    name.kind = win::PopupKind::text;
    name.value = L"On Save";
    const auto timer = SetTimer(owner, 2, 30, switch_open_menu);
    require(timer != 0, "Live menu timer unavailable");
    win::PopupMenuHost::track(owner, {50, 50}, win::tr(localization::TextKey::rope_settings), L"Save",
        {name, {831, win::tr(localization::TextKey::common_save)}});
    KillTimer(owner, timer);
    switching = nullptr;
    require(live_preserved, "Open editable menu lost its field or did not relabel");
}
}
int main() {
    const auto desktop = CreateDesktopW((L"ClipRope.Localization." + std::to_wstring(GetCurrentProcessId())).c_str(),
                                        nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop) || FAILED(OleInitialize(nullptr))) return 1;
    try {
        Directory temp;
        persistence::HistoryStore store(temp.path);
        persistence::Browser records(store);
        rendering::CardBrowser browser(records);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        const std::wstring contents = L"On";
        text.bytes.resize((contents.size() + 1) * sizeof(wchar_t));
        std::memcpy(text.bytes.data(), contents.c_str(), text.bytes.size());
        domain::Snapshot snapshot{{text}};
        snapshot.formats.push_back({0, "Save", {}, "skipped"});
        const auto entry = store.admit("main", snapshot);
        const auto raw = browser.summary(entry);
        require(raw.capabilities.empty() && raw.formats.size() == 1 && raw.text == contents,
                "History reader returned translated display labels");
        localization::LocalizationManager manager;
        win::UiLocalization binding(manager);
        const auto clipboard = GetClipboardSequenceNumber();
        const auto owner = win::create_ui_window(0, L"STATIC", win::tr(localization::TextKey::settings_window_title),
            WS_OVERLAPPEDWINDOW, 10, 10, 500, 300, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(owner != nullptr, "Could not create synthetic UI");
        win::fluent::Surface surface;
        surface.attach(owner);
        auto label = win::fluent::child(owner, L"STATIC", win::tr(localization::TextKey::common_save), 0, 1);
        auto untouched = win::fluent::child(owner, L"EDIT", L"Save", ES_MULTILINE, 2);
        const auto content_font = CreateFontW(-21, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH, L"Consolas");
        require(content_font != nullptr, "Synthetic content font unavailable");
        SendMessageW(untouched, WM_SETFONT, WPARAM(content_font), FALSE);
        auto nested = win::fluent::child(owner, L"STATIC",
            win::message(localization::TextKey::hotkey_enable, {{"action", win::tr(localization::TextKey::tray_quick_note)}}), 0, 3);
        SendMessageW(untouched, EM_SETSEL, 1, 3);
        SendMessageW(untouched, EM_SETMODIFY, TRUE, 0);
        toggle(owner, false);
        manager.set_language(localization::Language::simplified_chinese);
        SendMessageW(owner, win::language_changed, 0, 0);
        require(win::fluent::caption(label) == L"保存" && win::fluent::caption(nested) == L"启用快速笔记",
                "Native or nested labels did not refresh");
        DWORD begin{}, end{};
        SendMessageW(untouched, EM_GETSEL, WPARAM(&begin), LPARAM(&end));
        require(win::fluent::caption(untouched) == L"Save" && begin == 1 && end == 3 && SendMessageW(untouched, EM_GETMODIFY, 0, 0),
                "User text equal to a UI label was translated or its selection changed");
        require(SendMessageW(untouched, WM_GETFONT, 0, 0) == LRESULT(content_font),
                "Language refresh replaced the content editor's font");
        auto chinese = raw;
        presentation::localize_card(chinese, manager);
        require(chinese.text == L"On" && chinese.capabilities == L"文本 · 部分捕获", "User card text was translated");
        auto unavailable = raw;
        unavailable.text.clear();
        unavailable.text_unavailable = true;
        presentation::CardCapabilities capability(false);
        capability.retain(presentation::CardFormat::text);
        capability.apply(unavailable);
        presentation::localize_card(unavailable, manager);
        require(unavailable.status == application::CardStatus::damaged &&
                unavailable.text == manager.tr(localization::TextKey::card_unavailable),
                "Unreadable text used a raw or empty-content placeholder");
        const auto details = records.card_details(raw);
        bool registered_format{};
        for (const auto& line : details) {
            const auto [name, value] = presentation::detail_labels(line, manager);
            if (line.kind == application::DetailLine::format && name == L"Save") registered_format = value == L"已跳过";
        }
        require(registered_format, "Registered format name was translated or its outcome was not");
        toggle(owner, false);
        toggle(owner, true);
        live_menu(owner, manager);
        require(GetClipboardSequenceNumber() == clipboard && store.entry(entry).revision == raw.entry.revision &&
                store.active_rope() == "main", "Localization changed clipboard or history state");
        DestroyWindow(owner);
        DeleteObject(content_font);
        std::cout << "Bilingual labels, native selections, semantic reports and typed menu toggles passed\n";
        OleUninitialize();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        OleUninitialize();
        return 1;
    }
}
