#include "../compatibility/desktop_app.hpp"
#include "persistence/localization_settings.hpp"
#include "persistence/hotkey_settings.hpp"
#include "persistence/privacy.hpp"
#include "platform_win/tray_popup.hpp"
#include "platform_win/tray.hpp"
#include "platform_win/card_editor.hpp"
#include <ole2.h>
#include <psapi.h>
#include <uiautomation.h>
#include <wrl/client.h>

namespace {
using namespace workspace_fixture;
DWORD clipboard_sequence() {
    require(until([] { return OpenClipboard(nullptr) != FALSE; }), "Cannot open clipboard for observation");
    const auto sequence = GetClipboardSequenceNumber();
    // A fresh runner can have an accessible, empty clipboard with sequence zero.
    SetLastError(ERROR_SUCCESS);
    const bool observable = sequence != 0 || (EnumClipboardFormats(0) == 0 && GetLastError() == ERROR_SUCCESS);
    const bool closed = CloseClipboard() != FALSE;
    require(observable && closed, "Cannot observe clipboard sequence or confirm an empty clipboard");
    return sequence;
}

std::wstring text(HWND window) {
    wchar_t buffer[4096]{};
    SendMessageW(window, WM_GETTEXT, std::size(buffer), LPARAM(buffer));
    return buffer;
}
struct Observation {
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db{nullptr, sqlite3_close};
    explicit Observation(const fs::path& root) {
        const auto file = (root / "history.db").u8string();
        sqlite3* opened{};
        const auto result = sqlite3_open_v2(reinterpret_cast<const char*>(file.c_str()), &opened, SQLITE_OPEN_READWRITE, nullptr);
        db.reset(opened);
        require(result == SQLITE_OK, "Observation connection unavailable");
        sqlite3_busy_timeout(opened, 500);
    }
    std::string scalar(const char* sql) {
        Statement row(db.get(), sql);
        return row.step() ? row.text(0) : std::string{};
    }
    std::string history() {
        Statement rows(db.get(), "SELECT id,state,coalesce(rope_id,''),coalesce(sort_key,0),snapshot_id,revision "
                                  "FROM entries ORDER BY id");
        std::string result;
        while (rows.step()) for (int i = 0; i < 6; ++i) result += rows.text(i) + "|";
        Statement ropes(db.get(), "SELECT id,name,sort_rank FROM ropes ORDER BY id");
        while (ropes.step()) for (int i = 0; i < 3; ++i) result += ropes.text(i) + "|";
        result += scalar("SELECT active_rope_id FROM app_settings WHERE id=1");
        result += scalar("SELECT default_capacity||'|'||paste_on_double_click||'|'||text_font_size||'|'||"
                         "hover_resolution||'|'||image_file_previews||'|'||keep_expanded_group FROM app_settings WHERE id=1");
        result += scalar("SELECT paused||'|'||exclusions FROM capture_preferences WHERE id=1");
        result += scalar("SELECT topmost||'|'||undo_sign_in FROM general_preferences WHERE id=1");
        result += scalar("SELECT theme||'|'||match_value FROM appearance_preferences WHERE id=1");
        Statement shortcuts(db.get(), "SELECT action,enabled,chord FROM shortcut_preferences ORDER BY action");
        while (shortcuts.step()) for (int i = 0; i < 3; ++i) result += shortcuts.text(i) + "|";
        return result;
    }
};
void native_pattern(HWND control, PATTERNID pattern, const char* failure) {
    Microsoft::WRL::ComPtr<IUIAutomation> automation;
    Microsoft::WRL::ComPtr<IUIAutomationElement> element;
    Microsoft::WRL::ComPtr<IUnknown> provider;
    require(SUCCEEDED(CoCreateInstance(CLSID_CUIAutomation,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&automation))) &&
            SUCCEEDED(automation->ElementFromHandle(control,&element)) &&
            SUCCEEDED(element->GetCurrentPattern(pattern,&provider)) && provider,failure);
}
void select_language(HWND settings, HWND choice, int index) {
    SendMessageW(choice, CB_SETCURSEL, index, 0);
    SendMessageW(settings, WM_COMMAND, MAKEWPARAM(1940, CBN_SELCHANGE), LPARAM(choice));
}
HWND open_choice(HWND choice) {
    COMBOBOXINFO info{sizeof(info)};
    require(GetComboBoxInfo(choice, &info) != FALSE, "Language dropdown information unavailable");
    const auto arrow = MAKELPARAM((info.rcButton.left + info.rcButton.right) / 2,
                                 (info.rcButton.top + info.rcButton.bottom) / 2);
    PostMessageW(choice, WM_LBUTTONDOWN, MK_LBUTTON, arrow);
    PostMessageW(choice, WM_LBUTTONUP, 0, arrow);
    require(until([&] { return SendMessageW(choice, CB_GETDROPPEDSTATE, 0, 0) && IsWindowVisible(info.hwndList); }),
            "Clicking the language arrow did not open its list");
    RECT list{}, item{};
    require(GetClientRect(info.hwndList, &list) != FALSE, "Language list bounds unavailable");
    for (int i = 0; i < int(SendMessageW(choice, CB_GETCOUNT, 0, 0)); ++i) {
        require(SendMessageW(info.hwndList, LB_GETITEMRECT, i, LPARAM(&item)) != LB_ERR,
                "Language option rectangle unavailable");
        if (item.top < list.top || item.bottom > list.bottom)
            std::cerr << "Language option " << i << " extends to " << item.bottom
                      << " px in a " << list.bottom - list.top << " px list\n";
        require(item.bottom > item.top && item.top >= list.top && item.bottom <= list.bottom,
                "Language dropdown clips its options");
    }
    return info.hwndList;
}
void click_language(HWND choice, int index) {
    const auto list = open_choice(choice);
    RECT item{};
    require(SendMessageW(list, LB_GETITEMRECT, index, LPARAM(&item)) != LB_ERR,
            "Requested language option unavailable");
    const auto point = MAKELPARAM((item.left + item.right) / 2, (item.top + item.bottom) / 2);
    PostMessageW(list, WM_MOUSEMOVE, 0, point);
    PostMessageW(list, WM_LBUTTONDOWN, MK_LBUTTON, point);
    PostMessageW(list, WM_LBUTTONUP, 0, point);
    require(until([&] { return !SendMessageW(choice, CB_GETDROPPEDSTATE, 0, 0) &&
                              SendMessageW(choice, CB_GETCURSEL, 0, 0) == index; }),
            "Clicking a language option did not select it and close the list");
}
struct Resources { DWORD gdi, user; SIZE_T private_bytes; };
Resources resources(DWORD pid) {
    const auto process = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    require(process != nullptr, "Resource observer unavailable");
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    const auto ok = GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    const Resources result{GetGuiResources(process, GR_GDIOBJECTS), GetGuiResources(process, GR_USEROBJECTS), memory.PrivateUsage};
    CloseHandle(process);
    require(ok != FALSE, "Private Bytes unavailable");
    return result;
}
void capture(HWND window, const fs::path& folder, const char* name) {
    if (folder.empty()) return;
    brief(80ms);
    RECT rect{};
    GetWindowRect(window, &rect);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    BITMAPINFO info{};
    info.bmiHeader = {sizeof(BITMAPINFOHEADER), width, -height, 1, 32, BI_RGB};
    const auto dc = CreateCompatibleDC(nullptr);
    void* pixels{};
    const auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    require(dc && bitmap && pixels, "Window capture allocation failed");
    const auto original = SelectObject(dc, bitmap);
    const auto printed = PrintWindow(window, dc, PW_RENDERFULLCONTENT);
    const auto bytes = DWORD(width * height * 4);
    BITMAPFILEHEADER file{0x4d42, DWORD(sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) + bytes), 0, 0,
                          sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER)};
    fs::create_directories(folder);
    std::ofstream output(folder / (std::string(name) + ".bmp"), std::ios::binary);
    output.write(reinterpret_cast<const char*>(&file), sizeof(file));
    output.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
    output.write(static_cast<const char*>(pixels), bytes);
    SelectObject(dc, original);
    DeleteObject(bitmap);
    DeleteDC(dc);
    require(printed && bool(output), "Synthetic window capture failed");
}
void exclusions_check(DWORD pid, HWND settings, Observation& observer, const fs::path& snapshots) {
    const auto view = FindWindowExW(settings,nullptr,L"ClipRope.SettingsViewport",nullptr);
    auto open = [&] {
        PostMessageW(GetDlgItem(view,1944),BM_CLICK,0,0);
        HWND dialog{};
        require(until([&] { dialog=owned_window(pid,L"#32770"); return dialog && GetDlgItem(dialog,1975); }),
                "Exclusions dialog unavailable");
        return dialog;
    };
    auto dialog = open();
    auto list = GetDlgItem(dialog,1975);
    auto input = GetDlgItem(dialog,1972);
    auto count = [&] { return SendMessageW(list,LB_GETCOUNT,0,0); };
    auto add = [&](const std::wstring& value) {
        SendMessageW(input,WM_SETTEXT,0,LPARAM(value.c_str()));
        SendDlgItemMessageW(dialog,IDOK,BM_CLICK,0,0);
    };
    require(count()==0,"Exclusions did not start empty");
    native_pattern(list,UIA_SelectionPatternId,"Fluent list lost native Selection");
    capture(dialog,snapshots,"en-US-exclusions-empty");
    for (const auto* bad : {L"bad",L"C:\\bad.exe",L"*.exe",L"NUL.exe"}) {
        add(bad); require(count()==0,"Invalid executable was accepted");
    }
    add(L"Editor.exe"); add(L"EDITOR.EXE");
    require(count()==1,"Duplicate executable was accepted");
    for (int i=1;i<64;++i) add(L"Sample"+std::to_wstring(i)+L".exe");
    require(count()==64,"List did not retain 64 draft names");
    add(L"Overflow.exe"); require(count()==64,"Exclusions exceeded their limit");
    SendMessageW(input,WM_SETTEXT,0,LPARAM(L""));
    SendMessageW(list,WM_KEYDOWN,VK_END,0);
    require(SendMessageW(list,LB_GETCURSEL,0,0)==63 && SendMessageW(list,LB_GETTOPINDEX,0,0)>0,
            "List keyboard selection did not scroll to the last row");
    require(!(GetWindowLongPtrW(list,GWL_STYLE)&(WS_BORDER|WS_VSCROLL)),"List retains native chrome");
    auto bar = FindWindowExW(list,nullptr,L"ClipRope.ScrollBar",nullptr);
    require(bar && IsWindowVisible(bar),"Overflow list has no Fluent scroll bar");
    native_pattern(bar,UIA_RangeValuePatternId,"Fluent scroll bar lost RangeValue");
    capture(dialog,snapshots,"en-US-exclusions-full");
    SendMessageW(input,WM_SETTEXT,0,LPARAM(L"Pending.exe"));
    SendDlgItemMessageW(dialog,1977,BM_CLICK,0,0);
    require(IsWindow(dialog) && observer.scalar("SELECT exclusions FROM capture_preferences WHERE id=1").empty(),
            "Save ignored an unsubmitted name");
    SendMessageW(input,WM_SETTEXT,0,LPARAM(L""));
    require(sqlite3_exec(observer.db.get(),"CREATE TRIGGER reject_ui_list BEFORE UPDATE OF exclusions ON capture_preferences "
                       "BEGIN SELECT RAISE(ABORT,'fixture failure'); END",nullptr,nullptr,nullptr)==SQLITE_OK,
            "Could not inject list write failure");
    SendDlgItemMessageW(dialog,1977,BM_CLICK,0,0);
    require(IsWindow(dialog) && count()==64 && observer.scalar("SELECT exclusions FROM capture_preferences WHERE id=1").empty(),
            "Failed save lost draft or changed persisted exclusions");
    require(sqlite3_exec(observer.db.get(),"DROP TRIGGER reject_ui_list",nullptr,nullptr,nullptr)==SQLITE_OK,
            "Could not remove list fault");
    SendDlgItemMessageW(dialog,1977,BM_CLICK,0,0);
    require(until([&] { return !IsWindow(dialog); }),"Exclusions save did not close");
    dialog=open(); list=GetDlgItem(dialog,1975); input=GetDlgItem(dialog,1972);
    require(count()==64,"Saved exclusions did not reopen");
    while (count()) {
        SendMessageW(list,LB_SETCURSEL,0,0);
        SendMessageW(dialog,WM_COMMAND,MAKEWPARAM(1975,LBN_SELCHANGE),LPARAM(list));
        SendDlgItemMessageW(dialog,1976,BM_CLICK,0,0);
    }
    SendDlgItemMessageW(dialog,1977,BM_CLICK,0,0);
    require(until([&] { return !IsWindow(dialog); }),"Empty exclusions save did not close");
    dialog=open(); list=GetDlgItem(dialog,1975); input=GetDlgItem(dialog,1972);
    add(L"Cancelled.exe");
    SendMessageW(dialog,WM_CLOSE,0,0);
    require(observer.scalar("SELECT exclusions FROM capture_preferences WHERE id=1").empty(),"Cancel committed exclusions");
}
void settings_capture(HWND settings, const fs::path& folder, const std::string& language) {
    if (folder.empty()) return;
    capture(settings, folder, (language + "-general").c_str());
    const auto viewport = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
    const auto choice = GetDlgItem(viewport, 1940);
    capture(open_choice(choice), folder, (language + "-language-list").c_str());
    SendMessageW(choice, CB_SHOWDROPDOWN, FALSE, 0);
    RECT before{};
    GetWindowRect(settings, &before);
    SetWindowPos(settings, nullptr, 0, 0, win::fluent::px(settings, 680), win::fluent::px(settings, 480),
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE);
    capture(settings, folder, (language + "-general-narrow").c_str());
    capture(open_choice(choice), folder, (language + "-language-list-narrow").c_str());
    SendMessageW(choice, CB_SHOWDROPDOWN, FALSE, 0);
    SetWindowPos(settings, nullptr, 0, 0, win::fluent::px(settings, 1100), before.bottom - before.top,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE);
    RECT label{}, control{};
    GetWindowRect(FindWindowExW(viewport, nullptr, L"STATIC", language == "en-US" ? L"Language" : L"语言"), &label);
    GetWindowRect(choice, &control);
    require(std::abs(control.right - label.left - win::fluent::px(settings, 660)) <= 1,
            "Wide settings content did not stop at 660 DIP");
    capture(settings, folder, (language + "-general-wide").c_str());
    SetWindowPos(settings, nullptr, 0, 0, before.right-before.left, before.bottom-before.top,
                 SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE);
}
void cards_choice_check(HWND settings, Observation& observer, const fs::path& folder) {
    const auto view=FindWindowExW(settings,nullptr,L"ClipRope.SettingsViewport",nullptr);
    RECT original{}; GetWindowRect(settings,&original);
    for (int locale : {1,2}) {
        SendMessageW(settings,WM_COMMAND,1500,LPARAM(GetDlgItem(settings,1500)));
        select_language(settings,GetDlgItem(view,1940),locale);
        SendMessageW(settings,WM_COMMAND,1501,LPARAM(GetDlgItem(settings,1501)));
        const auto action=GetDlgItem(view,1920);
        const auto label=FindWindowExW(view,nullptr,L"STATIC",locale==1 ? L"Hanging double-click" : L"绳上卡片双击操作");
        require(action && label,"Card action row unavailable");
        for (bool narrow : {false,true}) {
            SetWindowPos(settings,nullptr,0,0,narrow ? win::fluent::px(settings,680) : original.right-original.left,
                         narrow ? win::fluent::px(settings,480) : original.bottom-original.top,
                         SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOMOVE);
            RECT a{},b{}; GetWindowRect(action,&a); GetWindowRect(label,&b);
            require(a.left>=b.right+win::fluent::px(settings,8) && a.top<b.bottom && a.bottom>b.top,
                    "Card action stacked below its label or overlapped the text column");
            const auto dc=GetDC(action);
            const auto font=SelectObject(dc,HFONT(SendMessageW(action,WM_GETFONT,0,0)));
            for (int item=0;item<2;++item) {
                wchar_t value[256]{}; SendMessageW(action,CB_GETLBTEXT,item,LPARAM(value));
                SIZE size{}; GetTextExtentPoint32W(dc,value,int(wcslen(value)),&size);
                require(size.cx<=a.right-a.left-win::fluent::px(settings,34),"Card action caption clips its chevron");
            }
            SIZE title{}; const auto caption=text(label);
            GetTextExtentPoint32W(dc,caption.c_str(),int(caption.size()),&title);
            SelectObject(dc,font); ReleaseDC(action,dc);
            require(title.cx<=b.right-b.left,"Card action heading clips in the inline layout");
            const auto name=std::string(locale==1 ? "en-US" : "zh-CN")+(narrow ? "-cards-narrow" : "-cards");
            capture(settings,folder,name.c_str());
            const auto list=open_choice(action);
            native_pattern(list,UIA_SelectionPatternId,"Card action list lost native Selection");
            capture(list,folder,(name+"-list").c_str());
            SendMessageW(action,WM_KEYDOWN,VK_ESCAPE,0);
            require(!SendMessageW(action,CB_GETDROPPEDSTATE,0,0),"Escape did not close the action list");
            SendMessageW(action,WM_KEYDOWN,VK_F4,0);
            require(SendMessageW(action,CB_GETDROPPEDSTATE,0,0),"F4 did not expand the action list");
            SendMessageW(action,WM_KEYDOWN,VK_ESCAPE,0);
        }
        native_pattern(action,UIA_ExpandCollapsePatternId,"Card action lost native ExpandCollapse");
        for (const auto id : {1929, 1930}) {
            const auto choice = GetDlgItem(view, id);
            const int selected = id == 1929 ? 0 : 1;
            require(choice && SendMessageW(choice, CB_GETCURSEL, 0, 0) == selected, "Wrong delete mode");
            native_pattern(choice, UIA_ExpandCollapsePatternId, "Deletion choice lost native ExpandCollapse");
            const auto list = open_choice(choice);
            native_pattern(list, UIA_SelectionPatternId, "Deletion list lost native Selection");
            capture(list, folder, (std::string(locale == 1 ? "en-US" : "zh-CN") + "-delete-" + std::to_string(id)).c_str());
            SendMessageW(choice, WM_KEYDOWN, VK_ESCAPE, 0);
            click_language(choice, 1 - selected);
            click_language(choice, selected);
        }
        click_language(action,1);
        require(observer.scalar("SELECT paste_on_double_click FROM app_settings WHERE id=1")=="0","Copy-only choice did not save");
        click_language(action,0);
        require(observer.scalar("SELECT paste_on_double_click FROM app_settings WHERE id=1")=="1","Paste choice did not save");
        exec(observer.db.get(),"CREATE TRIGGER fail_card_action BEFORE UPDATE ON app_settings BEGIN SELECT RAISE(ABORT,'Synthetic action failure'); END");
        SendMessageW(action,CB_SETCURSEL,1,0);
        SendMessageW(settings,WM_COMMAND,MAKEWPARAM(1920,CBN_SELCHANGE),LPARAM(action));
        require(SendMessageW(action,CB_GETCURSEL,0,0)==0 &&
                observer.scalar("SELECT paste_on_double_click FROM app_settings WHERE id=1")=="1",
                "Failed action save did not restore the committed choice");
        exec(observer.db.get(),"DROP TRIGGER fail_card_action");
        click_language(action,1); click_language(action,0);
        require(observer.scalar("SELECT paste_on_double_click FROM app_settings WHERE id=1")=="1","Action retry failed");
        if (!folder.empty()) {
            const char* names[] = {"ropes", "shortcuts", "appearance", "storage", "about"};
            SetWindowPos(settings, nullptr, 0, 0, original.right-original.left, original.bottom-original.top,
                         SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOMOVE);
            for (int i = 0; i < 5; ++i) {
                SendDlgItemMessageW(settings, 1502 + i, BM_CLICK, 0, 0);
                capture(settings, folder, (std::string(locale == 1 ? "en-US-" : "zh-CN-") + names[i]).c_str());
            }
            SendDlgItemMessageW(settings, 1501, BM_CLICK, 0, 0);
        }
    }
    SetWindowPos(settings,nullptr,0,0,original.right-original.left,original.bottom-original.top,
                 SWP_NOACTIVATE|SWP_NOZORDER|SWP_NOMOVE);
    std::cout << "Both card action rows stayed inline; native input/UIA, saves and rollback passed\n";
}
void edited_close(Desktop& app, HWND settings, DWORD pid, HWND floating, HWND editor, Observation& observer) {
    const auto view = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
    click_language(GetDlgItem(view, 1930), 0);
    PostMessageW(floating, WM_CONTEXTMENU, WPARAM(floating), MAKELPARAM(200, 200));
    HWND menu{};
    require(until([&] { menu = owned_window(pid, L"ClipRope.CardMenu"); return menu && IsWindowVisible(menu); }),
            "Floating text menu unavailable for deletion");
    PostMessageW(menu, WM_COMMAND, int(win::CardCommand::edit), LPARAM(GetDlgItem(menu, int(win::CardCommand::edit))));
    require(until([&] { return IsWindowVisible(editor); }), "Floating edit did not reopen");
    const wchar_t draft[] = L"Saved before direct deletion";
    const auto field = GetDlgItem(editor, 1760);
    SendMessageW(field, WM_SETTEXT, 0, LPARAM(draft));
    const auto original_notice = text(GetDlgItem(editor, 1761));
    const auto snapshot = [&] { return observer.scalar("SELECT snapshot_id FROM entries WHERE state='Floating'"); };
    const auto entry = observer.scalar("SELECT id FROM entries WHERE state='Floating'");
    const auto before = snapshot();
    RECT area{};
    GetClientRect(floating, &area);
    const auto inset = MulDiv(19, GetDpiForWindow(floating), 96);
    const auto close = [&] {
        const auto point = MAKELPARAM(area.right - inset, inset);
        SendMessageW(floating, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(floating, WM_LBUTTONUP, 0, point);
    };
    exec(observer.db.get(), "CREATE TRIGGER reject_draft BEFORE UPDATE OF snapshot_id ON entries "
                            "BEGIN SELECT RAISE(ABORT,'test edit failure'); END");
    close();
    require(until([&] { return IsWindowVisible(editor) && text(GetDlgItem(editor, 1761)) != original_notice; }) &&
            text(field) == draft && snapshot() == before, "A failed save lost the draft or closed its Floating card");
    exec(observer.db.get(), "DROP TRIGGER reject_draft; CREATE TRIGGER reject_close BEFORE DELETE ON entries "
                            "BEGIN SELECT RAISE(ABORT,'test delete failure'); END");
    close();
    require(until([&] { return app.contains("delete " + entry + " failed"); }) && !IsWindowVisible(editor) &&
            IsWindowVisible(floating) && !snapshot().empty() && snapshot() != before, "A failed deletion lost the saved Floating edit");
    exec(observer.db.get(), "DROP TRIGGER reject_close");
    close();
    require(until([&] { return !IsWindow(floating); }) && snapshot().empty() &&
            observer.scalar("SELECT count(*) FROM entries") == "1", "Retry did not delete only the edited card");
}

void run(const fs::path& executable, const fs::path& snapshots) {
    history_fixture::Directory temp;
    const auto profile = temp.path / "profile";
    fs::create_directory(profile);
    {
        HistoryStore store(profile);
        save_language(store.database(), localization::Language::english);
        save_workspace_topmost(store.database(), false);
        save_appearance(store.database(), {application::Theme::system, 0xe5a85e});
        save_shortcut(store.database(), application::workspace_toggle, {true, "Ctrl+Shift+F21"});
        save_shortcut(store.database(), application::quick_note, {false, "Ctrl+Shift+F21"});
        save_privacy(store.database(), {true, {}}); // Never capture the interactive station's clipboard.
        const auto rope = store.create_rope("Save-\xe5\x8e\x9f\xe5\xa7\x8b", 100);
        store.set_active_rope(rope);
        domain::Format format;
        format.standard_id = CF_UNICODETEXT;
        const std::wstring value = L"On Save 原始内容";
        format.bytes.resize((value.size()+1)*sizeof(wchar_t));
        std::memcpy(format.bytes.data(), value.c_str(), format.bytes.size());
        domain::Format html;
        html.name = "HTML Format";
        html.bytes = sample::formats(false).back().bytes;
        const auto hanging = store.admit(rope, {{format, html}});
        const auto floating = store.copy(hanging, rope);
        store.detach(floating, {"", 120, 160, 300, 180, true});
    }
    const auto started = std::chrono::steady_clock::now();
    Desktop app(executable, profile, true);
    DWORD pid{};
    GetWindowThreadProcessId(app.workspace, &pid);
    std::cout << "Ready in " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-started).count()
              << " ms, host Private Bytes " << resources(pid).private_bytes << '\n';
    const auto listener = owned_window(pid, L"ClipRope.Listener");
    Observation observer(profile);
    const auto settings_started = std::chrono::steady_clock::now();
    SendMessageW(listener, WM_COMMAND, win::tray_command::settings, 0);
    HWND settings{};
    require(until([&] { settings = owned_window(pid, L"ClipRope.Settings"); return settings && IsWindowVisible(settings); }),
            "Settings unavailable");
    require(IsDlgButtonChecked(settings, 1500) == BST_CHECKED, "First settings visit did not open General");
    if (!snapshots.empty()) {
        RedrawWindow(settings, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
        std::cout << "Settings first open/paint ms "
                  << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-settings_started).count()
                  << "; repeat open/paint ms";
        for (int repeat = 0; repeat != 5; ++repeat) {
            SendMessageW(settings, WM_CLOSE, 0, 0);
            const auto repeat_started = std::chrono::steady_clock::now();
            SendMessageW(listener, WM_COMMAND, win::tray_command::settings, 0);
            require(until([&] { settings = owned_window(pid, L"ClipRope.Settings"); return settings && IsWindowVisible(settings); }),
                    "Repeated Settings open failed");
            RedrawWindow(settings, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_UPDATENOW);
            std::cout << ' ' << std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-repeat_started).count();
        }
        std::cout << '\n';
    }
    SendMessageW(settings, WM_COMMAND, 1500, LPARAM(GetDlgItem(settings, 1500)));
    const auto viewport = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
    auto choice = GetDlgItem(viewport, 1940);
    require(choice && IsWindowEnabled(choice) && SendMessageW(choice, CB_GETCOUNT, 0, 0) == 3,
            "Language selector was not enabled");
    click_language(choice, 0);
    require(observer.scalar("SELECT language FROM localization_preferences WHERE id=1") == "auto",
            "Clicking Auto did not persist the preference");
    click_language(choice, 1);
    native_pattern(open_choice(choice),UIA_SelectionPatternId,"Fluent dropdown list lost native Selection");
    SendMessageW(choice,CB_SHOWDROPDOWN,FALSE,0);
    native_pattern(choice,UIA_ExpandCollapsePatternId,"Fluent choice lost native ExpandCollapse");
    require(!GetDlgItem(viewport, 1944), "General still exposes capture exclusions");
    SendDlgItemMessageW(settings, 1505, BM_CLICK, 0, 0);
    require(GetDlgItem(viewport, 1944) && !GetDlgItem(viewport, 1943), "Privacy action bound to the wrong page");
    exclusions_check(pid,settings,observer,snapshots);
    SendDlgItemMessageW(settings, 1503, BM_CLICK, 0, 0);
    SendDlgItemMessageW(viewport, 1804, BM_CLICK, 0, 0);
    const auto conflict = FindWindowExW(viewport, nullptr, L"STATIC", L"Used by Show or hide ClipRope.");
    require(conflict && IsWindowVisible(conflict) && !IsDlgButtonChecked(viewport, 1804),
            "Shortcut conflict did not keep the prior switch and explain the rejected change inline");
    capture(settings, snapshots, "en-US-shortcut-conflict");
    SendDlgItemMessageW(settings, 1500, BM_CLICK, 0, 0);
    choice = GetDlgItem(viewport, 1940);
    settings_capture(settings, snapshots, "en-US");
    if (!snapshots.empty()) {
        PostMessageW(settings, WM_COMMAND, 1943, LPARAM(GetDlgItem(viewport, 1943)));
        HWND prompt{};
        require(until([&] { prompt = owned_window(pid, L"#32770"); return prompt && IsWindowVisible(prompt); }),
                "English reset confirmation unavailable");
        capture(prompt, snapshots, "en-US-reset");
        PostMessageW(prompt, WM_COMMAND, IDCANCEL, LPARAM(GetDlgItem(prompt, IDCANCEL)));
        require(until([&] { return !IsWindow(prompt); }), "English reset did not cancel");
    }
    const auto floating = app.floating_window();
    require(floating != nullptr, "Saved Floating card did not reopen");
    PostMessageW(floating, WM_CONTEXTMENU, WPARAM(floating), MAKELPARAM(200, 200));
    HWND popup{};
    require(until([&] { popup = owned_window(pid, L"ClipRope.CardMenu"); return popup && IsWindowVisible(popup); }),
            "Card menu unavailable");
    capture(popup, snapshots, "en-US-card-menu");
    PostMessageW(popup, WM_COMMAND, int(win::CardCommand::information), LPARAM(GetDlgItem(popup, int(win::CardCommand::information))));
    HWND information{};
    require(until([&] { information = owned_window(pid, L"ClipRope.CardEditor"); return information && IsWindowVisible(information); }),
            "Information window unavailable");
    capture(information, snapshots, "en-US-information");
    PostMessageW(floating, WM_CONTEXTMENU, WPARAM(floating), MAKELPARAM(200, 200));
    require(until([&] { popup = owned_window(pid, L"ClipRope.CardMenu"); return popup && IsWindowVisible(popup); }),
            "Text card menu unavailable");
    PostMessageW(popup, WM_COMMAND, int(win::CardCommand::edit), LPARAM(GetDlgItem(popup, int(win::CardCommand::edit))));
    HWND editor{};
    require(until([&] { editor = owned_window(pid, L"ClipRope.InlineEditorHost"); return editor && IsWindowVisible(editor); }),
            "Floating text editor unavailable");
    require(text(GetDlgItem(editor, 1761)) == L"Saves as plain text", "Rich-edit notice was not shown");
    const auto field = GetDlgItem(editor, 1760);
    const std::wstring draft = L"Save 未保存的草稿\r\nSecond line";
    require(SendMessageW(field, WM_SETTEXT, 0, LPARAM(draft.c_str())) != FALSE, "Draft input unavailable");
    SendMessageW(field, EM_SETSEL, 1, 4);
    SendMessageW(field, WM_IME_STARTCOMPOSITION, 0, 0);
    const auto original_draft = text(field);
    const auto original_selection = SendMessageW(field, EM_GETSEL, 0, 0);
    const auto before = observer.history();
    const auto clipboard = clipboard_sequence();
    click_language(choice, 2);
    require(until([&] { return text(settings) == L"ClipRope 设置" && text(information) == L"卡片信息" &&
                              text(GetDlgItem(editor, IDOK)) == L"保存"; }), "Open windows did not switch to Chinese");
    require(observer.scalar("SELECT language FROM localization_preferences WHERE id=1") == "zh-CN",
            "Language preference was not committed");
    SendMessageW(listener, WM_SETTINGCHANGE, 0, 0);
    require(text(settings) == L"ClipRope 设置", "System notification overrode a manual language");
    require(field == GetDlgItem(editor, 1760), "Native field identity changed");
    require(text(GetDlgItem(editor, 1761)) == L"将保存为纯文本", "Rich-edit notice did not change language");
    require(text(field) == original_draft, "Language switch changed the draft text");
    require(SendMessageW(field, EM_GETSEL, 0, 0) == original_selection, "Language switch changed the native selection");
    require(text(GetDlgItem(information, 1701)).find(L"捕获详情") != std::wstring::npos,
            "Information report did not reformat its semantic labels");
    require(observer.history() == before && clipboard_sequence() == clipboard &&
            listener == owned_window(pid, L"ClipRope.Listener"), "Switch changed history, clipboard or listener identity");
    settings_capture(settings, snapshots, "zh-CN");
    capture(information, snapshots, "zh-CN-information");
    capture(editor, snapshots, "zh-CN-draft");
    SendMessageW(field, WM_IME_ENDCOMPOSITION, 0, 0);
    click_language(choice, 1);
    require(until([&] { return text(settings) == L"ClipRope Settings"; }), "English override did not apply");
    exec(observer.db.get(), "CREATE TRIGGER fail_ui_language BEFORE UPDATE ON localization_preferences "
                            "BEGIN SELECT RAISE(ABORT,'Synthetic language failure'); END");
    select_language(settings, choice, 2);
    require(text(settings) == L"ClipRope Settings" && SendMessageW(choice, CB_GETCURSEL, 0, 0) == 1 &&
            observer.scalar("SELECT language FROM localization_preferences WHERE id=1") == "en-US",
            "Failed save changed the language or selection");
    exec(observer.db.get(), "DROP TRIGGER fail_ui_language");
    require(observer.history() == before && text(field) == original_draft, "Failed switch changed history or draft");
    select_language(settings, choice, 2);
    require(until([&] { return text(settings) == L"ClipRope 设置"; }), "Language retry failed");
    auto switch_repeatedly = [&](int count) { for (int i = 0; i < count; ++i) {
        const auto language = i % 2 ? 2 : 1;
        select_language(settings, choice, language);
        require(until([&] { return text(settings) == (language == 1 ? L"ClipRope Settings" : L"ClipRope 设置"); }),
                "Repeated language switch did not refresh");
    } };
    switch_repeatedly(20); // Warm both native text and fallback-font caches before measuring retention.
    brief(200ms);
    const auto start_resources = resources(pid);
    switch_repeatedly(40);
    brief(200ms);
    const auto end_resources = resources(pid);
    std::cout << "40 switches: GDI " << start_resources.gdi << " -> " << end_resources.gdi
              << ", USER " << start_resources.user << " -> " << end_resources.user
              << ", Private Bytes " << start_resources.private_bytes << " -> " << end_resources.private_bytes << '\n';
    require(end_resources.gdi <= start_resources.gdi + 2 && end_resources.user <= start_resources.user + 2 &&
            end_resources.private_bytes <= start_resources.private_bytes + 4u*1024*1024, "Repeated switching retained UI resources");
    require(observer.history() == before && text(field) == original_draft, "Repeated switching changed history or draft");
    SendMessageW(editor, WM_COMMAND, IDCANCEL, 0);
    require(until([&] { return !IsWindowVisible(editor); }), "Synthetic draft did not cancel");
    if (!snapshots.empty()) {
        PostMessageW(floating, WM_CONTEXTMENU, WPARAM(floating), MAKELPARAM(200, 200));
        require(until([&] { popup = owned_window(pid, L"ClipRope.CardMenu"); return popup && IsWindowVisible(popup); }),
                "Chinese card menu unavailable");
        capture(popup, snapshots, "zh-CN-card-menu");
        PostMessageW(popup, WM_KEYDOWN, VK_ESCAPE, 0);
        require(until([&] { return !IsWindow(popup); }), "Chinese card menu did not cancel");
    }
    cards_choice_check(settings,observer,snapshots);
    require(observer.history()==before,"Action layout checks changed history or committed settings");
    edited_close(app, settings, pid, floating, editor, observer);
    app.stop();
    require(app.clean_exit, "Desktop did not exit cleanly");
    {
        Desktop restarted(executable, profile, false);
        DWORD next_pid{};
        GetWindowThreadProcessId(restarted.workspace, &next_pid);
        SendMessageW(owned_window(next_pid, L"ClipRope.Listener"), WM_COMMAND, win::tray_command::settings, 0);
        HWND next{};
        require(until([&] { next = owned_window(next_pid, L"ClipRope.Settings"); return next && text(next) == L"ClipRope 设置"; }),
                "Language did not survive process restart");
        SendMessageW(next, WM_COMMAND, 1500, LPARAM(GetDlgItem(next, 1500)));
        const auto next_view = FindWindowExW(next, nullptr, L"ClipRope.SettingsViewport", nullptr);
        const auto reset = GetDlgItem(next_view, 1943);
        PostMessageW(next, WM_COMMAND, 1943, LPARAM(reset));
        HWND prompt{};
        require(until([&] { prompt = owned_window(next_pid, L"#32770"); return prompt && IsWindowVisible(prompt); }),
                "Reset confirmation unavailable");
        require(text(GetDlgItem(prompt, 1712)).find(L"语言将恢复为自动") != std::wstring::npos,
                "Reset did not explain the language change");
        capture(prompt, snapshots, "zh-CN-reset");
        PostMessageW(prompt, WM_COMMAND, IDOK, LPARAM(GetDlgItem(prompt, IDOK)));
        require(until([&] { return observer.scalar("SELECT count(*) FROM localization_preferences") == "0" &&
                                  SendMessageW(GetDlgItem(next_view, 1940), CB_GETCURSEL, 0, 0) == 0; }),
                "Global reset did not restore Auto");
        require(SendMessageW(GetDlgItem(next_view, 1940), CB_GETCURSEL, 0, 0) == 0, "Reset did not update the language selector");
        SendMessageW(next, WM_COMMAND, 1501, LPARAM(GetDlgItem(next, 1501)));
        require(observer.scalar("SELECT count(*) FROM card_delete_preferences") == "0" &&
                SendDlgItemMessageW(next_view, 1929, CB_GETCURSEL, 0, 0) == 0 &&
                SendDlgItemMessageW(next_view, 1930, CB_GETCURSEL, 0, 0) == 1, "Reset lost the card deletion defaults");
        restarted.stop();
        require(restarted.clean_exit, "Restarted desktop did not exit cleanly");
    }
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 2 && (argc != 4 || std::wstring_view(argv[2]) != L"--snapshots")) return 2;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto desktop = CreateDesktopW((L"ClipRope.LanguageDesktop." + std::to_wstring(GetCurrentProcessId())).c_str(),
                                        nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop) || FAILED(OleInitialize(nullptr))) return 2;
    try { run(argv[1], argc == 4 ? fs::path(argv[3]) : fs::path{});
        std::cout << "Live language switching, draft/IME routing, rollback, restart and reset passed\n"; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; OleUninitialize(); return 1; }
    OleUninitialize();
    return 0;
}
