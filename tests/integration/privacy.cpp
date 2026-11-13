#include "../compatibility/desktop_app.hpp"
#include "../clipboard_sources/external_copy.hpp"
#include "persistence/privacy.hpp"
#include "platform_win/history_clipboard.hpp"

namespace {
using namespace workspace_fixture;
constexpr wchar_t secret[] = L"PRIVATE-SEED-410 C:\\Users\\Synthetic\\Private\\report.pdf";
std::string contents(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
HWND settings(const Desktop &host) {
    DWORD pid{};
    GetWindowThreadProcessId(host.workspace, &pid);
    auto listener = owned_window(pid, L"ClipRope.Listener");
    SendMessageW(listener, WM_COMMAND, 30, 0);
    auto window = owned_window(pid, L"ClipRope.Settings");
    SendDlgItemMessageW(window,1505,BM_CLICK,0,0);
    auto page = FindWindowExW(window, nullptr, L"ClipRope.SettingsViewport", nullptr);
    require(page && GetDlgItem(page, 1904) && !IsWindowEnabled(GetDlgItem(page, 1904)) && GetDlgItem(page, 1907)
            && !GetDlgItem(page, 1410), "Explicit profile must display storage usage and lock the folder choice");
    return window;
}
HWND exclusion_editor(HWND settings) {
    SendDlgItemMessageW(settings, 1505, BM_CLICK, 0, 0);
    auto page = FindWindowExW(settings, nullptr, L"ClipRope.SettingsViewport", nullptr);
    auto button = GetDlgItem(page, 1944);
    require(button && IsWindowEnabled(button) && PostMessageW(page, WM_COMMAND, MAKEWPARAM(1944, BN_CLICKED), LPARAM(button)),
            "Storage & Privacy did not expose the Edit list action");
    HWND editor{};
    require(until([&] {
        editor = FindWindowExW(nullptr, nullptr, L"#32770", L"Clipboard capture exclusions");
        return editor && GetWindow(editor, GW_OWNER) == settings && GetDlgItem(editor, 1977);
    }, 10s), "Capture exclusions editor did not open");
    return editor;
}
void copy_event(const fs::path &root, const wchar_t *producer, const wchar_t *mode, std::string_view status) {
    auto report = root / "desktop-test.txt";
    const auto before = contents(report).size();
    probe::ExternalCopy copy(producer, mode);
    require(until(
                [&] {
                    auto log = contents(report);
                    return log.size() >= before &&
                           log.find("capture " + std::string(status), before) != log.npos;
                },
                5s),
            "Missing expected capture outcome");
    if (status == "excluded-marker" || status == "paused" || status == "excluded-app")
        require(WaitForSingleObject(copy.reading, 0) == WAIT_TIMEOUT, "Excluded source payload was read");
}
void run(const wchar_t *desktop, const wchar_t *producer, const wchar_t *headless) {
    history_fixture::Directory dir;
    auto profile = dir.path / "profile";
    std::string hanging, floating, other;
    {
        HistoryStore store(profile);
        const auto *bytes = reinterpret_cast<const unsigned char *>(secret);
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes.assign(bytes, bytes + sizeof(secret));
        hanging = store.admit("main", {{text}});
        floating = store.copy(hanging, "main");
        store.detach(floating, {"", 0, 0, 240, 160, false});
        other = store.create_rope("Second", 10);
        store.copy(hanging, other);
        store.set_active_rope("main");
    }
    auto policy=[&](application::CapturePolicy value) {
        HistoryStore store(profile);
        persistence::save_privacy(store.database(),value);
    };
    policy({true,{}});
    {
        Desktop host(desktop,profile);
        auto window=settings(host);
        SendMessageW(window,WM_COMMAND,1412,0);
        copy_event(profile,producer,L"paused","paused");
    }
    const auto renamed=dir.path / L"Éditeur.exe";
    require(fs::copy_file(producer,renamed),"Could not stage the localized producer");
    policy({false,{"unused.exe","éditeur.exe","CLIPROPE_OLE_SOURCE.EXE"}});
    {
        Desktop host(desktop,profile);
        copy_event(profile,renamed.c_str(),L"paused","excluded-app");
        copy_event(profile,producer,L"paused","excluded-app");
        require(order(profile)==Order{hanging},"Paused or excluded capture changed history");
    }
    policy({});
    {
        Desktop host(desktop, profile, true);
        auto window = settings(host);
        auto editor = exclusion_editor(window);
        const auto executable = fs::path(producer).filename();
        require(SendDlgItemMessageW(editor, 1972, WM_SETTEXT, 0, LPARAM(executable.c_str())),
                "Could not enter the source name");
        SendMessageW(editor, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), LPARAM(GetDlgItem(editor, IDOK)));
        require(SendDlgItemMessageW(editor, 1975, LB_GETCOUNT, 0, 0) == 1, "Add did not stage the producer");
        copy_event(profile, producer, L"exclude", "excluded-marker");
        SendMessageW(editor, WM_COMMAND, MAKEWPARAM(1977, BN_CLICKED), LPARAM(GetDlgItem(editor, 1977)));
        require(until([&] { return !IsWindow(editor); }), "Saving exclusions failed");
        editor = exclusion_editor(window);
        require(SendDlgItemMessageW(editor, 1975, LB_GETCOUNT, 0, 0) == 1,
                "Saved exclusion was missing on reopen");
        copy_event(profile, producer, L"no-cloud", "excluded-app");
        auto list = GetDlgItem(editor, 1975);
        SendMessageW(list, LB_SETCURSEL, 0, 0);
        SendMessageW(editor, WM_COMMAND, MAKEWPARAM(1975, LBN_SELCHANGE), LPARAM(list));
        SendMessageW(editor, WM_COMMAND, MAKEWPARAM(1976, BN_CLICKED), LPARAM(GetDlgItem(editor, 1976)));
        copy_event(profile, producer, L"no-cloud", "excluded-app");
        SendMessageW(editor, WM_COMMAND, MAKEWPARAM(1977, BN_CLICKED), LPARAM(GetDlgItem(editor, 1977)));
        require(until([&] { return !IsWindow(editor); }), "Removing the exclusion failed");
        copy_event(profile, producer, L"exclude", "excluded-marker");
        copy_event(profile, producer, L"no-history", "excluded-marker");
        require(order(profile) == Order{hanging}, "Local exclusion marker admitted a card");
        copy_event(profile, producer, L"no-cloud", "captured");
        const auto entries = order(profile);
        require(entries.size() == 2 && entries.back() == hanging, "Cloud-only marker prevented local capture");
        copy_event(profile, producer, L"failed", "no-usable-format");
        SendMessageW(window,WM_COMMAND,1413,0);
        require(!fs::exists(profile/"diagnostics.txt"),"Read-only page exported a file");
        const auto log=contents(profile/"desktop-test.txt");
        const auto* data=&log;
            for (std::string_view marker : {"PRIVATE-SEED", "Synthetic\\Private", "C:\\Users", "<b>Sample",
                                            "secret.exe", "CLIPROPE_OLE_SOURCE", "report.pdf"})
                require(data->find(marker) == data->npos, "Sensitive diagnostic material");
        host.stop();
        require(host.clean_exit, "Second resident failed to exit");
    }
    // Start another normal publisher from disk and consume its Unicode bytes independently.
    HANDLE child = probe::launch({headless, profile.wstring(), L"head"}, probe::desktop_path());
    require(child != nullptr, "Could not start local recall");
    require(until([&] { return WaitForSingleObject(child, 0) == WAIT_OBJECT_0; }, 6s),
            "Local recall timed out");
    DWORD code{};
    GetExitCodeProcess(child, &code);
    CloseHandle(child);
    require(code == 0, "Local recall failed");
    const auto version = GetClipboardSequenceNumber();
    auto read = capture_clipboard(version);
    require(read.saved == sample::formats(false), "Local publication bytes changed");
    if (OpenClipboard(nullptr)) {
        if (GetClipboardSequenceNumber() == version)
            EmptyClipboard();
        CloseClipboard();
    }
    HistoryStore store(profile);
    auto clear = [&](application::RopeOp op, const std::string &rope = {}) {
        auto consent = store.preview_rope_change({op, rope});
        store.apply_rope_change(consent);
        store.gc();
    };
    clear(application::RopeOp::clear, "main");
    require(store.hanging("main").empty() && store.hanging(other).size() == 1 &&
                store.floating().front().id == floating,
            "Rope clear affected another owner");
    clear(application::RopeOp::clear_all);
    require(store.hanging(other).empty() && store.floating().front().id == floating,
            "All-rope clear removed Floating");
    clear(application::RopeOp::delete_all);
    require(store.floating().empty(), "Explicit confirmed erase retained Floating");
    std::cout << "Pause, exclusion across restart, privacy markers, local recall, scoped clears "
                 "and live exclusion edits passed.\n";
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc != 4 || !probe::private_station() || FAILED(OleInitialize(nullptr)))
        return 2;
    int result{};
    try {
        run(argv[1], argv[2], argv[3]);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    OleUninitialize();
    return result;
}
