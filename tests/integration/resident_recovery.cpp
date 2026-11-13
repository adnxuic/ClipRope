#include "resident.hpp"
#include "history_fixture.hpp"
#include "platform_win/tray.hpp"
#include "platform_win/tray_popup.hpp"
#include "persistence/history_store.hpp"
#include "persistence/hotkey_settings.hpp"
#include "persistence/privacy.hpp"
#include "../compatibility/management_frame.hpp"
#include <iostream>

namespace {
using namespace cliprope;
using probe::require;
bool shown(HWND window) { return window && (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE); }
void invoke_note(probe::Resident& host) {
    DWORD_PTR result{};
    require(SendMessageTimeoutW(host.listener(), WM_COMMAND, win::tray_command::quick_note, 0,
                                SMTO_ABORTIFHUNG, 2000, &result) != 0, "Quick note invocation did not return");
}
void note(const wchar_t* exe, const wchar_t* fault, bool before_commit, const std::filesystem::path& frames) {
    history_fixture::Directory dir;
    { persistence::HistoryStore seed(dir.path); persistence::save_privacy(seed.database(), {true, {}}); }
    const auto sequence = GetClipboardSequenceNumber();
    {
        probe::Resident host(exe, dir.path, false, dir.path / "report.txt", fault);
        invoke_note(host);
        if (before_commit) {
            require(!probe::owned_window(host.pid(), L"ClipRope.Floating"), "Failed precommit left a window");
        } else {
            HWND paper{}, editor{};
            probe::pump_until([&] {
                paper = probe::owned_window(host.pid(), L"ClipRope.Floating");
                editor = probe::owned_window(host.pid(), L"ClipRope.InlineEditorHost");
                return shown(paper) && shown(editor) && GetWindow(editor, GW_OWNER) == paper;
            }, "Committed note did not recover its editor");
            management_fixture::frame(editor, frames / (std::wstring(fault ? fault : L"note-normal") + L".bmp"));
            auto field = GetDlgItem(editor, 1760);
            require(field && PostMessageW(field, WM_KEYDOWN, VK_ESCAPE, 0),
                    "Could not cancel the original edit");
            probe::pump_until([&] { return !shown(editor); }, "Cancel did not finish");
            invoke_note(host);
            probe::pump_until([&] { return shown(editor) && GetWindow(editor, GW_OWNER) != paper; },
                              "A later deliberate invocation reused the original note");
        }
        host.exit();
    }
    persistence::HistoryStore reopened(dir.path);
    const auto notes = reopened.floating();
    require(notes.size() == (before_commit ? 0u : 2u), "Unexpected note count after restart");
    for (const auto& entry : notes) require(reopened.layout(entry.id).visible &&
        reopened.begin_text_edit(entry.id).text.empty() && !entry.rope_id, "Original Floating draft changed");
    require(GetClipboardSequenceNumber() == sequence, "Quick note replaced the clipboard");
}

enum class ShellFault { add_modify, version, healthy };
ShellFault shell_fault;
unsigned adds{}, modifications{}, versions{}, deletes{};
BOOL WINAPI shell(DWORD message, PNOTIFYICONDATAW icon) {
    if (icon->uID != win::Tray::icon_id) return FALSE;
    switch (message) {
    case NIM_ADD: ++adds; return shell_fault != ShellFault::add_modify;
    case NIM_MODIFY: ++modifications; return shell_fault != ShellFault::add_modify;
    case NIM_SETVERSION: ++versions; return shell_fault == ShellFault::healthy;
    case NIM_DELETE: ++deletes; return TRUE;
    default: return TRUE;
    }
}
void tray(const wchar_t* exe, const std::filesystem::path& frames) {
    history_fixture::Directory dir;
    {
        persistence::HistoryStore store(dir.path);
        persistence::save_shortcut(store.database(), application::workspace_toggle, {false, "Ctrl+Alt+F11"});
        store.admit("main", {{{CF_UNICODETEXT, {}, {'T',0,'r',0,'a',0,'y',0,0,0}}}});
    }
    // A noninteractive station has no Shell icon. A disabled global shortcut leaves the workspace as the entry point.
    {
        probe::Resident host(exe, dir.path, false, dir.path / "report.txt", L"tray-unavailable", true);
        if (!frames.empty()) std::filesystem::copy_file(dir.path / "report.bmp", frames / "tray-fallback.bmp",
                                                         std::filesystem::copy_options::overwrite_existing);
        host.exit();
    }
    if (!frames.empty()) {
        probe::Resident visible(exe, dir.path, true, dir.path / "normal.txt", L"tray-frame");
        std::filesystem::copy_file(dir.path / "normal.bmp", frames / "tray-open.bmp",
                                   std::filesystem::copy_options::overwrite_existing);
        visible.exit();
    }
    {
        persistence::HistoryStore store(dir.path);
        require(!persistence::read_shortcuts(store.database())[application::workspace_toggle].enabled,
                "Recovery changed the saved shortcut");
    }
    HWND listener = CreateWindowExW(0,L"STATIC",L"tray fixture",WS_POPUP,0,0,10,10,nullptr,nullptr,nullptr,nullptr);
    require(listener != nullptr, "Could not create fixture owner");
    unsigned opens{};
    win::TrayActions actions;
    actions.open_workspace = [&] { ++opens; };
    actions.read_shortcuts = [] {
        auto bindings = application::default_shortcuts();
        for (auto& key : bindings) key.enabled = false;
        return bindings;
    };
    {
        win::Tray entry(std::move(actions), shell, true);
        shell_fault = ShellFault::add_modify;
        require(entry.start(listener) && !entry.routes().reachable() && adds == 1 && modifications == 1,
                "Failed add/modify reported a usable route");
        shell_fault = ShellFault::version;
        const auto restart = RegisterWindowMessageW(L"TaskbarCreated");
        entry.message(restart,0,0);
        require(!entry.routes().reachable() && versions == 1 && deletes == 1 && opens == 1,
                "Version failure left a ghost icon or no route");
        shell_fault = ShellFault::healthy;
        entry.message(restart,0,0);
        require(entry.routes().icon && !entry.routes().workspace_hotkey && opens == 1 && adds == 3 && versions == 2,
                "Shell recovery changed the shortcut or reopened the workspace");
    }
    require(deletes == 2, "Recovered icon did not retire exactly once");
    DestroyWindow(listener);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc < 3 || argc > 4) return 2;
    const bool check_tray = std::wstring_view(argv[2]) == L"tray";
    if (check_tray && !probe::private_station()) return 2;
    HDESK original = GetThreadDesktop(GetCurrentThreadId()), desk{};
    if (!check_tray) {
        // Only the synthetic note desktop owns these HWNDs. Never activate it or publish to the station clipboard.
        const auto name = L"ClipRope.NoteRecovery." + std::to_wstring(GetCurrentProcessId());
        desk = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
        if (!desk || !SetThreadDesktop(desk)) return 2;
    }
    int status{};
    try {
        const std::filesystem::path frames = argc == 4 ? argv[3] : L"";
        if (!frames.empty()) { std::filesystem::create_directories(frames); management_fixture::record_frames = true; }
        if (check_tray) tray(argv[1], frames);
        else {
            note(argv[1], nullptr, false, frames);
            note(argv[1], L"note-window", true, frames); note(argv[1], L"note-commit", true, frames);
            note(argv[1], L"note-summary", false, frames); note(argv[1], L"note-editor", false, frames);
        }
        std::cout << "Resident recovery passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; status = 1; }
    if (desk) { SetThreadDesktop(original); CloseDesktop(desk); }
    return status;
}
