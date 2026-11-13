#pragma once
#include "delivery_fixture.hpp"
#include "../integration/clipboard_process.hpp"
#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace desktop_fixture {
using fixture::Action;
using fixture::Query;
using namespace std::chrono_literals;

inline void require(bool value, const char* what) {
    if (!value) throw std::runtime_error(what);
}

template<class F>
bool until(F done, std::chrono::milliseconds timeout = 3s) {
    const auto end = std::chrono::steady_clock::now() + timeout;
    do {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (done()) return true;
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    } while (std::chrono::steady_clock::now() < end);
    return false;
}

inline DWORD_PTR request(HWND hwnd, UINT msg, WPARAM w = 0) {
    DWORD_PTR result{};
    require(SendMessageTimeoutW(hwnd, msg, w, 0, SMTO_ABORTIFHUNG | SMTO_ERRORONEXIT,
                                1800, &result) != 0, "fixture request timed out");
    return result;
}

struct Fields {
    HANDLE process{};
    DWORD pid{};
    HWND window{};
    void start() {
        wchar_t path[MAX_PATH]{};
        const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
        require(length && length < MAX_PATH, "fixture executable path unavailable");
        process = probe::launch({path, L"--fields", std::to_wstring(GetCurrentProcessId())}, probe::desktop_path());
        require(process != nullptr, "could not start fields process");
        pid = GetProcessId(process);
        require(until([&] {
            while ((window = FindWindowExW(nullptr, window, fixture::class_name, nullptr))) {
                DWORD owner{};
                GetWindowThreadProcessId(window, &owner);
                if (owner == pid) return true;
            }
            return false;
        }), "fixture window unavailable");
    }
    void act(Action a) const { require(request(window, fixture::command, static_cast<WPARAM>(a)) == 1, "fixture action failed"); }
    DWORD_PTR get(Query q) const { return request(window, fixture::inspect, static_cast<WPARAM>(q)); }
    ~Fields() {
        if (window) PostMessageW(window, WM_CLOSE, 0, 0);
        if (process) { WaitForSingleObject(process, 1500); CloseHandle(process); }
    }
};

inline void input_click(HWND card, RECT rect, HWND target) {
    POINT center{(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2};
    require(SetCursorPos(center.x, center.y) != 0, "could not locate probe card");
    require(until([&] { return GetForegroundWindow() == target && WindowFromPoint(center) == card; }),
            "probe windows do not own input");
    INPUT click[2]{};
    click[0].type = click[1].type = INPUT_MOUSE;
    click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    require(SendInput(2, click, sizeof(INPUT)) == 2, "first click was refused");
    const auto next = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::min<UINT>(60, GetDoubleClickTime()/4));
    until([&] { return std::chrono::steady_clock::now() >= next; });
    require(SendInput(2, click, sizeof(INPUT)) == 2, "second click was refused");
}

inline void input_paste(HWND target) {
    // Give clipboard listeners time to release the publication and the target
    // time to process key-down. Send exactly one chord without rendering data.
    auto quiet_since = std::chrono::steady_clock::now();
    auto sequence = GetClipboardSequenceNumber();
    require(until([&] {
        const auto now = std::chrono::steady_clock::now();
        const auto current = GetClipboardSequenceNumber();
        const bool available = OpenClipboard(nullptr) && CloseClipboard();
        if (sequence != current || !available) quiet_since = now;
        sequence = current;
        return available && now - quiet_since >= 50ms;
    }), "clipboard busy before manual paste");
    require(GetForegroundWindow() == target, "fixture lost foreground before Ctrl+V");
    INPUT keys[4]{};
    const WORD codes[] = {VK_CONTROL, 'V', 'V', VK_CONTROL};
    for (int i = 0; i < 4; ++i) {
        keys[i].type = INPUT_KEYBOARD;
        keys[i].ki.wVk = codes[i];
        keys[i].ki.dwFlags = i >= 2 ? KEYEVENTF_KEYUP : 0;
    }
    require(SendInput(2, keys, sizeof(INPUT)) == 2, "Ctrl+V press was refused");
    const auto release = std::chrono::steady_clock::now() + 50ms;
    until([&] { return std::chrono::steady_clock::now() >= release; });
    require(SendInput(2, keys + 2, sizeof(INPUT)) == 2, "Ctrl+V release was refused");
}

inline void mouse(DWORD flags) {
    INPUT event{};
    event.type = INPUT_MOUSE;
    event.mi.dwFlags = flags;
    require(SendInput(1, &event, sizeof event) == 1, "Mouse event failed");
}
inline void reset_fields(const Fields& fields) {
    RECT work{};
    require(SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) != FALSE, "Test work area unavailable");
    require(SetWindowPos(fields.window, HWND_TOPMOST, work.left + 32, work.bottom - 240, 0, 0,
                         SWP_NOSIZE | SWP_NOACTIVATE) != FALSE, "Could not position the synthetic fields");
    if (GetForegroundWindow() != fields.window) {
        POINT point{30, 65};
        ClientToScreen(fields.window, &point);
        require(GetAncestor(WindowFromPoint(point), GA_ROOT) == fields.window, "Synthetic target is covered");
        require(SetCursorPos(point.x, point.y) != FALSE, "Could not select the synthetic target");
        mouse(MOUSEEVENTF_LEFTDOWN); mouse(MOUSEEVENTF_LEFTUP);
        require(until([&] { return GetForegroundWindow() == fields.window; }), "Field selection did not take focus");
    }
    fields.act(Action::reset);
    require(until([&] { return GetForegroundWindow() == fields.window; }), "Expected the synthetic fields to own foreground");
}
} // namespace desktop_fixture
