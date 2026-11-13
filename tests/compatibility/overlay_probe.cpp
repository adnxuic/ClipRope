#include "platform_win/overlay.hpp"
#include "rendering/probe_card.hpp"

#include <shellscalingapi.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {
constexpr auto receiver_class = L"ClipRope.Probe.Receiver";
constexpr UINT read_clicks = WM_APP + 1;
constexpr COLORREF gap_color = RGB(240, 224, 191);
struct Monitor { RECT work; UINT dpi; };
enum class Mode { isolated, physical, manual };

BOOL CALLBACK list_monitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    MONITORINFO info{sizeof(info)};
    UINT x = 0, y = 0;
    if (!GetMonitorInfoW(monitor, &info) ||
        FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) return FALSE;
    auto& monitors = *reinterpret_cast<std::vector<Monitor>*>(context);
    monitors.push_back({info.rcWork, x});
    std::printf("monitor=%zu dpi=%u work=(%ld,%ld,%ld,%ld)\n", monitors.size(), x,
                info.rcWork.left, info.rcWork.top, info.rcWork.right, info.rcWork.bottom);
    return TRUE;
}

void pump() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

template<class Predicate>
bool until(Predicate ready, DWORD timeout_ms = 3000) {
    const auto deadline = GetTickCount64() + timeout_ms;
    do {
        pump();
        if (ready()) return true;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 15, QS_ALLINPUT);
    } while (GetTickCount64() < deadline);
    return false;
}

LRESULT CALLBACK receiver_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, GetWindowLongPtrW(hwnd, GWLP_USERDATA) + 1);
        return 0;
    case read_clicks:
        return GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(hwnd, &ps);
        SetDCBrushColor(dc, gap_color);
        FillRect(dc, &ps.rcPaint, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int receiver(DWORD parent_id) {
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parent_id);
    if (!parent) return 2;
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = receiver_proc;
    wc.lpszClassName = receiver_class;
    const std::wstring title = L"ClipRope underlay " + std::to_wstring(parent_id);
    if (!RegisterClassW(&wc)) { CloseHandle(parent); return 2; }
    HWND hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
                                receiver_class, title.c_str(), WS_POPUP,
                                0, 0, 1, 1, nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { CloseHandle(parent); return 2; }
    MSG msg{};
    while (MsgWaitForMultipleObjects(1, &parent, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { CloseHandle(parent); return 0; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    DestroyWindow(hwnd);
    CloseHandle(parent);
    return 0;
}

struct ReceiverProcess {
    HANDLE process = nullptr;
    HWND window = nullptr;
    bool start(wchar_t* desktop) {
        wchar_t exe[32768]{};
        if (!GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)))) return false;
        const auto pid = std::to_wstring(GetCurrentProcessId());
        auto args = std::wstring(L"\"") + exe + L"\" --receiver " + pid;
        const auto title = L"ClipRope underlay " + pid;
        STARTUPINFOW startup{sizeof(startup)};
        startup.lpDesktop = desktop;
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(exe, args.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                            nullptr, nullptr, &startup, &child)) return false;
        CloseHandle(child.hThread);
        process = child.hProcess;
        return until([&] {
            const HWND found = FindWindowW(receiver_class, title.c_str());
            DWORD owner = 0;
            if (found) GetWindowThreadProcessId(found, &owner);
            if (owner == child.dwProcessId) window = found;
            return window != nullptr || WaitForSingleObject(process, 0) == WAIT_OBJECT_0;
        }, 5000) && window;
    }
    ~ReceiverProcess() {
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        if (process) {
            WaitForSingleObject(process, 2000);
            CloseHandle(process);
        }
    }
    int clicks() const {
        DWORD_PTR count = 0;
        if (!SendMessageTimeoutW(window, read_clicks, 0, 0, SMTO_ABORTIFHUNG, 500, &count)) return -1;
        return static_cast<int>(count);
    }
};

bool queue_click(HWND target, POINT screen) {
    if (!ScreenToClient(target, &screen)) return false;
    const auto pos = MAKELPARAM(screen.x, screen.y);
    return PostMessageW(target, WM_LBUTTONDOWN, MK_LBUTTON, pos) &&
           PostMessageW(target, WM_LBUTTONUP, 0, pos);
}

bool inject_click(HWND expected, POINT screen) {
    // Abort if anything covers the fixture, the pointer moved, or a button is already down.
    if (!SetCursorPos(screen.x, screen.y)) { std::puts("cannot-position-cursor"); return false; }
    POINT cursor{};
    if (!GetCursorPos(&cursor) || cursor.x != screen.x || cursor.y != screen.y) {
        std::puts("cursor-moved"); return false;
    }
    if (WindowFromPoint(cursor) != expected) { std::puts("target-changed"); return false; }
    if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000)) {
        std::puts("button-busy"); return false;
    }
    INPUT inputs[2]{};
    inputs[0].type = inputs[1].type = INPUT_MOUSE;
    inputs[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    inputs[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (SendInput(2, inputs, sizeof(INPUT)) != 2) { std::puts("input-denied"); return false; }
    return true;
}

bool color_at(POINT point, COLORREF color) {
    HDC screen = GetDC(nullptr);
    if (!screen) return false;
    const auto pixel = GetPixel(screen, point.x, point.y);
    ReleaseDC(nullptr, screen);
    return pixel == color;
}

bool test_click(Mode mode, const ReceiverProcess& receiver_process, HWND card, int& presses,
                POINT point, bool expect_card, const char* stage) {
    const HWND expected = expect_card ? card : receiver_process.window;
    const HWND hit = WindowFromPoint(point);
    DWORD pid = 0;
    GetWindowThreadProcessId(hit, &pid);
    std::printf("stage=%s point=(%ld,%ld) expect=%s hit=%s pid=%lu ",
                stage, point.x, point.y, expect_card ? "card" : "gap",
                hit == card ? "card" : hit == receiver_process.window ? "underlay" : "other", pid);
    if (hit != expected) { std::puts("wrong-hit"); return false; }
    const auto card_before = presses;
    const auto gap_before = receiver_process.clicks();
    if (gap_before < 0) return false;
    if (mode != Mode::isolated) {
        const auto color = expect_card ? cliprope::rendering::probe_card_color : gap_color;
        if (!until([&] { return color_at(point, color); })) {
            std::puts("visible-pixel-mismatch"); return false;
        }
    }
    if (mode == Mode::manual) { std::puts("click the indicated point"); std::fflush(stdout); }
    if (mode == Mode::physical && !inject_click(expected, point)) return false;
    if (mode == Mode::isolated && !queue_click(expected, point)) return false;
    const bool observed = until([&] {
        return presses != card_before || receiver_process.clicks() != gap_before;
    }, mode == Mode::manual ? 60000 : 3000);
    pump();
    const int delta_card = presses - card_before, delta_gap = receiver_process.clicks() - gap_before;
    std::printf("received(card=%d,underlay=%d)\n", delta_card, delta_gap);
    return observed && delta_card == (expect_card ? 1 : 0) && delta_gap == (expect_card ? 0 : 1);
}

int exercise(Mode mode, const std::vector<Monitor>& monitors, wchar_t* desktop) {
    ReceiverProcess underlay;
    if (!underlay.start(desktop)) { std::puts("underlay launch failed"); return 1; }
    int presses = 0;
    cliprope::win::Overlay overlay({[&] { ++presses; }, [&] { ++presses; }});
    bool mixed = false;
    for (const auto& m : monitors) {
        const auto px = [&](int dip) { return MulDiv(dip, m.dpi, 96); };
        const POINT origin{(m.work.left + m.work.right - px(320)) / 2,
                           (m.work.top + m.work.bottom - px(154)) / 2};
        if (!SetWindowPos(underlay.window, HWND_TOPMOST, origin.x, origin.y, px(320), px(154),
                          SWP_NOACTIVATE | SWP_SHOWWINDOW)) return 1;
        if (!overlay.window() ? !overlay.open(origin) : !overlay.move_to(origin)) return 1;
        if (!SetWindowPos(overlay.window(), HWND_TOPMOST, 0, 0, 0, 0,
                          SWP_NOACTIVATE | SWP_NOMOVE | SWP_NOSIZE)) return 1;
        if (GetDpiForWindow(overlay.window()) != m.dpi) { std::puts("DPI mismatch"); return 1; }
        if (!overlay.set_right_slot(false)) return 1;
        const RECT left = overlay.card_screen_rect();
        const POINT card_point{left.left + 2, left.top + 2};
        const POINT other_point{left.left + px(164) + 2, left.top + 2};
        const POINT edge{left.right, left.top + 2};
        if (!test_click(mode, underlay, overlay.window(), presses, card_point, true, "left-card") ||
            !test_click(mode, underlay, overlay.window(), presses, edge, false, "right-edge-gap") ||
            !test_click(mode, underlay, overlay.window(), presses, other_point, false, "right-slot-gap") ||
            !overlay.set_right_slot(true) ||
            !test_click(mode, underlay, overlay.window(), presses, card_point, false, "vacated-left-slot") ||
            !test_click(mode, underlay, overlay.window(), presses, other_point, true, "moved-card")) return 1;
        mixed |= m.dpi != monitors.front().dpi;
    }
    std::printf("monitor-count=%zu mixed-dpi=%s\n", monitors.size(), mixed ? "exercised" : "unavailable");
    return 0;
}

int isolated(const std::vector<Monitor>& monitors) {
    auto name = L"ClipRope.Overlay." + std::to_wstring(GetCurrentProcessId());
    auto old = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop) {
        std::printf("private desktop creation failed (win32=%lu)\n", GetLastError());
        return 1;
    }
    int result = 1;
    if (SetThreadDesktop(desktop)) {
        result = exercise(Mode::isolated, monitors, name.data());
        SetThreadDesktop(old);
    }
    CloseDesktop(desktop);
    return result;
}
} // namespace

int main(int argc, char** argv) {
    if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2) &&
        !AreDpiAwarenessContextsEqual(GetThreadDpiAwarenessContext(), DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        std::puts("per-monitor v2 unavailable"); return 2;
    }
    const std::string_view mode = argc > 1 ? argv[1] : "";
    if (mode == "--receiver" && argc == 3) return receiver(std::stoul(argv[2]));
    std::vector<Monitor> monitors;
    if (!EnumDisplayMonitors(nullptr, nullptr, list_monitor, reinterpret_cast<LPARAM>(&monitors)) ||
        monitors.empty()) return 2;
    if (mode == "--topology") return 0;
    if (mode == "--isolated") return isolated(monitors);
    if (mode == "--physical" || mode == "--manual") {
        POINT cursor{};
        if (!GetCursorPos(&cursor)) return 1;
        const int result = exercise(mode == "--physical" ? Mode::physical : Mode::manual,
                                    monitors, nullptr);
        if (mode == "--physical") SetCursorPos(cursor.x, cursor.y);
        return result;
    }
    std::puts("Use --topology, --isolated, --physical, or --manual.");
    return 2;
}
