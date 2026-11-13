#include "../integration/history_fixture.hpp"
#include "../integration/resident.hpp"
#include "../clipboard_sources/sample.hpp"
#include "persistence/history_store.hpp"
#include "platform_win/performance_counters.hpp"
#include "platform_win/rope_placement.hpp"
#include "platform_win/workspace.hpp"
#include <wtsapi32.h>
#include <dwmapi.h>
#include <algorithm>
#include <fstream>
#include <chrono>
#include <optional>
#include <sstream>

using namespace cliprope;
using probe::require;
namespace {
DWORD_PTR send(HWND window, UINT message, WPARAM w = 0, LPARAM l = 0) {
    DWORD_PTR result{};
    require(SendMessageTimeoutW(window, message, w, l, SMTO_ABORTIFHUNG, 3000, &result) != 0,
            "Window did not answer notification");
    return result;
}
bool reachable(HWND window) {
    RECT rect{};
    MONITORINFO monitor{sizeof(monitor)};
    return GetWindowRect(window, &rect) &&
           GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONULL), &monitor) &&
           rect.left >= monitor.rcWork.left && rect.top >= monitor.rcWork.top &&
           rect.right <= monitor.rcWork.right && rect.bottom <= monitor.rcWork.bottom;
}
bool shown(HWND window) {
    return (GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE) != 0;
}

void print_client(HWND window) {
    // A noninteractive station has no compositor driving WM_PAINT. PrintWindow
    // asks the external owner to paint its ordinary WM_PRINTCLIENT path.
    RECT bounds{};
    GetClientRect(window,&bounds);
    BITMAPINFO info{};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=bounds.right;
    info.bmiHeader.biHeight=-bounds.bottom;
    info.bmiHeader.biPlanes=1;
    info.bmiHeader.biBitCount=32;
    HDC dc=CreateCompatibleDC(nullptr);
    void* bits{};
    auto bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bits,nullptr,0);
    require(dc && bitmap && bits,"Cannot allocate notification fixture surface");
    auto old=SelectObject(dc,bitmap);
    const bool printed=PrintWindow(window,dc,PW_CLIENTONLY)!=FALSE;
    SelectObject(dc,old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    require(printed,"Could not request external native paint");
}

void replay(probe::Resident &app, HWND floating_hwnd) {
    DWORD session{};
    require(ProcessIdToSessionId(app.pid(), &session) != FALSE, "Could not identify fixture session");
    require(SetWindowPos(floating_hwnd, nullptr, -24000, -15000, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER) != FALSE &&
                !reachable(floating_hwnd),
            "Fixture could not place Floating window outside desktop");
    send(app.listener(), WM_WTSSESSION_CHANGE, WTS_REMOTE_CONNECT, session + 1);
    require(!reachable(floating_hwnd), "Different session changed this session's layout");
    const auto created = [&] {
        return send(app.listener(), win::performance_query,
                    static_cast<WPARAM>(win::PerformanceCounter::render_target_creations));
    };
    const auto repaint = [&] {
        if (shown(app.workspace))
            print_client(app.workspace);
        print_client(floating_hwnd);
    };
    repaint();
    const auto first = created();
    require(first > 0, "No Direct2D target created before recovery");
    const auto dpi = GetDpiForWindow(app.workspace);
    const auto paper = MAKELPARAM(MulDiv(55, dpi, 96), MulDiv(140, dpi, 96));
    const auto sequence = GetClipboardSequenceNumber();
    send(app.workspace, WM_LBUTTONDOWN, MK_LBUTTON, paper);
    send(app.listener(), WM_WTSSESSION_CHANGE, WTS_REMOTE_DISCONNECT, session);
    send(app.listener(), WM_WTSSESSION_CHANGE, WTS_REMOTE_CONNECT, session);
    send(app.workspace, WM_LBUTTONDBLCLK, MK_LBUTTON, paper);
    send(app.workspace, WM_LBUTTONUP, 0, paper);
    require(shown(app.workspace) && sequence == GetClipboardSequenceNumber(),
            "Expired gesture published after reconnect");
    require(reachable(floating_hwnd), "Reconnect did not rescue off-screen Floating card");
    repaint();
    require(created() > first, "Reconnect did not recreate native render target");
    const auto reconnected = created();
    send(app.listener(), WM_DWMCOMPOSITIONCHANGED);
    repaint();
    require(created() > reconnected, "Compositor invalidation did not recreate native render target");
    send(app.listener(), WM_DISPLAYCHANGE, 32, MAKELPARAM(1280, 720));
    send(app.listener(), WM_SETTINGCHANGE, SPI_SETWORKAREA);
    send(app.workspace, WM_CLOSE);
    require(!shown(app.workspace), "Workspace did not hide");
    const auto before_resume = created();
    send(app.listener(), WM_POWERBROADCAST, PBT_APMSUSPEND);
    send(app.listener(), WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC);
    send(app.listener(), WM_WTSSESSION_CHANGE, WTS_SESSION_LOCK, session);
    send(app.listener(), WM_WTSSESSION_CHANGE, WTS_SESSION_UNLOCK, session);
    repaint();
    require(created() > before_resume && !shown(app.workspace) && shown(floating_hwnd) &&
                reachable(floating_hwnd),
            "Resume or unlock lost Floating visibility or reopened the hidden workspace");
}

struct PowerReceipt {
    bool subscribed{}, removed{};
    std::uint64_t suspend{}, resume{};
};
PowerReceipt read_receipt(const std::filesystem::path &path) {
    PowerReceipt events;
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
        if (line == "power subscribed")
            events.subscribed = true;
        else if (line == "power unsubscribed")
            events.removed = true;
        else if (events.subscribed && !events.resume && line.starts_with("power ")) {
            std::istringstream fields(line);
            std::string prefix, kind;
            std::uint64_t ticks{};
            if (fields >> prefix >> kind >> ticks && ticks) {
                if (kind == "suspend")
                    events.suspend = ticks;
                else if ((kind == "resume-automatic" || kind == "resume-user") && events.suspend &&
                         ticks >= events.suspend)
                    events.resume = ticks;
            }
        }
    }
    return events;
}

// This opt-in path sends no input or power messages. The timeout is wall time.
// A sleeping computer first gets to observe expiry when it is next awake.
bool await_natural_resume(const probe::Resident &app, const std::filesystem::path &receipt, unsigned seconds) {
    using namespace std::chrono;
    FILETIME start{};
    GetSystemTimeAsFileTime(&start);
    const auto start_ticks = (static_cast<std::uint64_t>(start.dwHighDateTime) << 32) | start.dwLowDateTime;
    const auto deadline_ticks = start_ticks + static_cast<std::uint64_t>(seconds) * 10000000;
    const auto deadline = system_clock::now() + std::chrono::seconds(seconds);
    require(read_receipt(receipt).subscribed, "Windows suspend/resume registration unavailable");
    std::cout << "natural_resume=armed timeout_seconds=" << seconds << std::endl;
    for (;;) {
        require(WaitForSingleObject(app.handle(), 0) == WAIT_TIMEOUT,
                "Candidate exited while awaiting natural sleep");
        const auto observed = read_receipt(receipt);
        if (observed.resume && observed.resume <= deadline_ticks) {
            std::cout << "suspend_utc_filetime=" << observed.suspend
                      << " resume_utc_filetime=" << observed.resume << std::endl;
            return true;
        }
        const auto left = duration_cast<milliseconds>(deadline - system_clock::now()).count();
        if (left <= 0)
            return false;
        WaitForSingleObject(app.handle(), static_cast<DWORD>(std::min<std::int64_t>(left, 10000)));
    }
}

int notifications(const wchar_t *host, const wchar_t *source, unsigned natural_wait = 0) {
    history_fixture::Directory profile;
    std::string hanging, floating, main;
    {
        persistence::HistoryStore seed(profile.path);
        main = seed.active_rope();
        domain::Format text;
        text.standard_id = CF_UNICODETEXT;
        text.bytes = {'L', 0, 0, 0};
        domain::Snapshot sample;
        sample.formats.push_back(text);
        hanging = seed.admit(main, sample);
        floating = seed.copy(hanging, main);
        seed.detach(floating, {"Detached monitor", -24000, -15000, 240, 160});
    }
    std::filesystem::path receipt;
#ifdef CLIPROPE_DESKTOP_PROBES
    receipt = profile.path / "power-receipt.txt";
#endif
    probe::Resident app(host, profile.path, !natural_wait, receipt);
    const auto floating_hwnd = probe::owned_window(app.pid(), L"ClipRope.Floating");
    require(floating_hwnd && reachable(floating_hwnd),
            "Floating card did not restore into the surviving work area");
    if (natural_wait) {
        require(!receipt.empty(), "Natural wake qualification requires a probe-enabled candidate");
        if (!await_natural_resume(app, receipt, natural_wait)) {
            app.exit();
            std::cout << "natural_resume=timeout qualifying_power_pair=0 external_copies=0\n";
            return 3;
        }
    } else
        replay(app, floating_hwnd);
    require(!shown(app.workspace) && shown(floating_hwnd) && reachable(floating_hwnd),
            "Post-resume layout changed");
    const auto dpi = GetDpiForWindow(app.workspace);
    {
        std::optional<probe::ExternalCopy> clipboard_reader;
        if (!natural_wait) {
            clipboard_reader.emplace(source, L"reader-250");
            SetEvent(clipboard_reader->proceed);
            require(WaitForSingleObject(clipboard_reader->reading, 3000) == WAIT_OBJECT_0,
                    "Could not establish temporary post-resume clipboard contention");
        }
        probe::ExternalCopy copy(source, L"after-resume");
        const auto path = (profile.path / L"history.db").u8string();
        sqlite3 *raw{};
        require(sqlite3_open_v2(reinterpret_cast<const char *>(path.c_str()), &raw, SQLITE_OPEN_READONLY,
                                nullptr) == SQLITE_OK,
                "Could not observe recovered listener");
        std::unique_ptr<sqlite3, decltype(&sqlite3_close)> reader(raw, sqlite3_close);
        sqlite3_busy_timeout(raw, 500);
        probe::pump_until(
            [&] {
                persistence::Statement count(raw, "SELECT count(*) FROM entries WHERE rope_id=?");
                count.bind(1, main);
                return count.step() && count.integer(0) == 2;
            },
            "Resumed listener did not capture the next independent copy");
    }
    app.exit();
#ifdef CLIPROPE_DESKTOP_PROBES
    const auto observed = read_receipt(receipt);
    require(observed.subscribed && observed.suspend && observed.resume && observed.removed,
            "Power subscription, delivered messages or cleanup missing from qualification receipt");
    std::cout << "power_subscription=registered suspend_and_resume_receipts=ordered shutdown=unsubscribed\n";
#endif
    persistence::HistoryStore checked(profile.path);
    require(checked.hanging(main).size() == 2 && checked.hanging(main).back().id == hanging &&
                checked.floating().size() == 1 && checked.floating().front().id == floating,
            "Recovered history identities changed");
    const auto content = checked.load(checked.hanging(main).front().id);
    const auto expected = sample::formats(false).front().bytes;
    require(std::any_of(content.snapshot.formats.begin(), content.snapshot.formats.end(),
                        [&](const auto &format) {
                            return format.outcome == "saved" && format.standard_id == CF_UNICODETEXT &&
                                   format.bytes == expected;
                        }),
            "Post-resume copy did not contain the independent publisher's synthetic bytes");
    if (natural_wait) {
        std::cout << "natural_resume=observed hidden_workspace=1 same_reachable_floating=1 "
                     "exact_post_wake_copy=1 clean_shutdown=1\n";
        return 0;
    }
    std::cout << "session_notifications=disconnect/reconnect/lock/unlock other_session_ignored=1 "
                 "power=suspend/resume "
              << "stale_gesture_publications=0 removed_monitor_rescued=1 compositor_target_recreated=1 "
                 "hidden_listener_next_copies=1 station_dpi="
              << dpi << '\n';
    return 0;
}

void dpi_changes() {
    application::CardSummary sample;
    sample.entry.id = "stable";
    win::WorkspaceReads reads{[](auto) { return 1u; },
                              [&](auto, std::size_t start, std::size_t count) {
                                  return !start && count ? std::vector<application::CardSummary>{sample}
                                                         : std::vector<application::CardSummary>{};
                              },
                              [](auto, auto) { return std::optional<std::size_t>{0}; },
                              [](auto) { return application::Anchor{}; }, [](auto, auto &) {}};
    win::Workspace view(std::move(reads), {});
    require(view.open(), "Could not open DPI fixture");
    view.show("main");
    long unit_height{};
    for (int dpi : {96, 144, 192, 96}) {
        const RECT left_monitor{-1600, -300, 0, 850};
        const auto clamped = win::rope_placement(left_monitor, dpi, 264, 2000, false);
        require(clamped.left == -1600 && clamped.right == 0 && clamped.bottom == 850 &&
                clamped.top == 850 - MulDiv(264, dpi, 96), "Negative-monitor clamp lost the rope or cards");
        RECT r{-600, -30, -600 + MulDiv(800, dpi, 96), -30 + MulDiv(220, dpi, 96)};
        send(view.window(), WM_DPICHANGED, MAKEWPARAM(dpi, dpi), reinterpret_cast<LPARAM>(&r));
        RECT actual{};
        GetWindowRect(view.window(), &actual);
        MONITORINFO monitor{sizeof(monitor)};
        require(GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &monitor) &&
                actual.left == monitor.rcWork.left && actual.right == monitor.rcWork.right &&
                actual.top == monitor.rcWork.top + MulDiv(24, dpi, 96) && actual.bottom <= monitor.rcWork.bottom,
                "Workspace lost its saved offset or full width after a DPI change");
        const auto slots = view.slots();
        require(slots.size() == 1 && slots.front().first == "stable",
                "DPI transition recycled a visible identity");
        const auto paper = slots.front().second;
        if (!unit_height)
            unit_height = paper.bottom - paper.top;
        require(std::abs((paper.bottom - paper.top) - MulDiv(unit_height, dpi, 96)) <= 1,
                "Physical paper height did not track DPI");
        HRGN region = CreateRectRgn(0, 0, 0, 0);
        require(region && GetWindowRgn(view.window(), region) != ERROR, "DPI region unavailable");
        const auto center_x = (paper.left + paper.right) / 2 - actual.left,
                   center_y = (paper.top + paper.bottom) / 2 - actual.top;
        const bool hit = PtInRegion(region, center_x, center_y) != FALSE;
        const bool gap = PtInRegion(region, actual.right - actual.left - 8, center_y) == FALSE;
        DeleteObject(region);
        require(hit && gap, "DPI transition lost paper or admitted transparent gap");
    }
    std::cout << "WM_DPICHANGED=96/144/192/96 negative_monitor_clamped=1 stable_slot_identity=1 "
                 "paper_geometry_scaled=1 hit_region=paper_in/gap_out\n";
}
} // namespace
int wmain(int argc, wchar_t **argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (!probe::private_station() || FAILED(OleInitialize(nullptr)))
        return 2;
    int result = 0;
    try {
        if (argc == 2 && std::wstring_view(argv[1]) == L"--dpi")
            dpi_changes();
        else if (argc == 3)
            result = notifications(argv[1], argv[2]);
        else if (argc == 5 && std::wstring_view(argv[1]) == L"--await-natural-resume") {
            wchar_t *end{};
            const auto seconds = wcstoul(argv[2], &end, 10);
            require(end != argv[2] && !*end && seconds > 0 && seconds <= 86400,
                    "Use a timeout in seconds from 1 to 86400");
            result = notifications(argv[3], argv[4], seconds);
        } else
            result = 2;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    OleUninitialize();
    return result;
}
