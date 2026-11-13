#pragma once
#include "desktop_fixture.hpp"
#include "../clipboard_sources/sample.hpp"
#include "../integration/history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "persistence/browse.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>

namespace workspace_fixture {
using namespace desktop_fixture;
using namespace cliprope;
using namespace cliprope::persistence;
namespace fs = std::filesystem;
using Order = std::vector<std::string>;

inline bool clipboard_text_is(std::wstring_view expected) {
    if (!OpenClipboard(nullptr)) return false;
    auto memory = GetClipboardData(CF_UNICODETEXT);
    auto text = memory ? static_cast<const wchar_t*>(GlobalLock(memory)) : nullptr;
    const bool matches = text && GlobalSize(memory) >= (expected.size() + 1) * sizeof(wchar_t) &&
                         std::wstring_view(text, expected.size()) == expected && !text[expected.size()];
    if (text) GlobalUnlock(memory);
    CloseClipboard();
    return matches;
}

inline HWND owned_window(DWORD pid, const wchar_t* type) {
    HWND hwnd{};
    while ((hwnd = FindWindowExW(nullptr, hwnd, type, nullptr))) {
        DWORD owner{};
        GetWindowThreadProcessId(hwnd, &owner);
        if (pid == owner) return hwnd;
    }
    return nullptr;
}

struct Frame {
    std::vector<std::pair<std::string, RECT>> cards;
    std::vector<std::pair<std::string, RECT>> rows;
};

class Desktop {
public:
    Desktop(const fs::path& exe, const fs::path& profile, bool visible = false, const wchar_t* fault = nullptr,
            bool defer_floating = false, bool export_diagnostics = false)
        : report_(profile / "desktop-test.txt") {
        std::vector<std::wstring> args{exe.wstring(), L"--profile", profile.wstring(), L"--probe", report_.wstring()};
        if (visible) args.emplace_back(L"--show");
        if (export_diagnostics) args.emplace_back(L"--export-diagnostics-on-exit");
        if (fault) { args.emplace_back(L"--fault"); args.emplace_back(fault); }
        if (defer_floating) args.emplace_back(L"--defer-floating");
        std::ofstream(report_, std::ios::trunc).close();
        process_ = probe::launch(args, probe::desktop_path());
        require(process_ != nullptr, "Desktop host did not start");
        const DWORD pid = GetProcessId(process_);
        if (!until([&] {
            listener_ = owned_window(pid, L"ClipRope.Listener");
            workspace = owned_window(pid, L"ClipRope.Workspace");
            return listener_ && workspace && contains("ready");
        }, 8s)) {
            stop();
            throw std::runtime_error("Host listener or ready event unavailable");
        }
    }
    ~Desktop() {
        if (std::uncaught_exceptions()) {
            std::ifstream log(report_);
            std::string line;
            while (std::getline(log, line))
                if (!line.starts_with("slot ") && !line.starts_with("frame ") && !line.starts_with("endframe "))
                    std::cerr << line << '\n';
        }
        stop();
    }
    Desktop(const Desktop&) = delete;
    Desktop& operator=(const Desktop&) = delete;
    void stop() {
        if (!process_) return;
        if (listener_) PostMessageW(listener_, WM_CLOSE, 0, 0);
        if (!until([&] { return WaitForSingleObject(process_, 0) == WAIT_OBJECT_0; }, 4s)) {
            TerminateProcess(process_, 3);
            WaitForSingleObject(process_, 3000);
            clean_exit = false;
        } else {
            DWORD code{};
            clean_exit = GetExitCodeProcess(process_, &code) && code == 0;
        }
        CloseHandle(process_);
        process_ = nullptr;
    }
    bool contains(std::string_view marker) const {
        std::ifstream report(report_);
        std::string line;
        while (std::getline(report, line)) if (line.find(marker) != line.npos) return true;
        return false;
    }
    HWND floating_window() const { return owned_window(GetProcessId(process_), L"ClipRope.Floating"); }
    Frame frame() const {
        std::ifstream report(report_);
        Frame complete, next;
        std::string line;
        while (std::getline(report, line)) {
            std::istringstream input(line);
            std::string kind, id;
            input >> kind;
            if (kind == "frame") next = {};
            else if (kind == "endframe") complete = next;
            else if (kind == "slot" || kind == "row") {
                RECT rc{};
                if (input >> id >> rc.left >> rc.top >> rc.right >> rc.bottom)
                    (kind == "slot" ? next.cards : next.rows).emplace_back(id, rc);
            }
        }
        return complete;
    }
    RECT card(const std::string& id) const {
        RECT result{};
        require(until([&] {
            if (!IsWindowVisible(workspace)) return false;
            for (const auto& [entry, rect] : frame().cards) {
                if (entry == id) { result = rect; return true; }
            }
            return false;
        }), "Visible card geometry unavailable");
        return result;
    }
    void check_display(const Order& ids) const {
        require(until([&] {
            Order actual;
            for (const auto& slot : frame().cards) actual.push_back(slot.first);
            return IsWindowVisible(workspace) && actual == ids;
        }), "Displayed card identities or order did not match");
    }
    HWND workspace{};
    bool clean_exit = false;
private:
    fs::path report_;
    HWND listener_{};
    HANDLE process_{};
};

inline Order order(const fs::path& profile, std::string_view rope = "main") {
    const auto path = (profile / "history.db").u8string();
    sqlite3* raw{};
    if (sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()), &raw, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(raw);
        throw std::runtime_error("Could not open observation connection");
    }
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, sqlite3_close);
    sqlite3_busy_timeout(raw, 500);
    Statement rows(raw, "SELECT id FROM entries WHERE state='Hanging' AND rope_id=? ORDER BY sort_key,id");
    rows.bind(1, rope);
    Order result;
    while (rows.step()) result.push_back(rows.text(0));
    return result;
}

inline std::string external_copy(ClipboardPublisher& producer, const fs::path& profile, wchar_t letter) {
    const auto before = order(profile);
    auto formats = sample::formats(false);
    if (letter != L'C') {
        const wchar_t value[] = {letter, 0};
        formats.resize(1);
        formats[0] = {CF_UNICODETEXT, std::vector<unsigned char>(sizeof value)};
        std::memcpy(formats[0].bytes.data(), value, sizeof value);
    }
    require(SUCCEEDED(producer.publish(formats)), "External fixture copy failed");
    Order current;
    if (!until([&] {
        current = order(profile);
        return !current.empty() && std::find(before.begin(), before.end(), current.front()) == before.end();
    }, 5s)) {
        std::ifstream diagnostics(profile / "desktop-test.txt");
        std::cerr << "Copy " << static_cast<char>(letter) << " did not reach history. Host events:\n"
                  << diagnostics.rdbuf();
        throw std::runtime_error("External copy did not reach history");
    }
    std::cout << static_cast<char>(letter) << '=' << current.front() << '\n';
    return current.front();
}

inline void key(WORD code, bool down) {
    INPUT event{};
    event.type = INPUT_KEYBOARD;
    event.ki.wVk = code;
    event.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    require(SendInput(1, &event, sizeof event) == 1, "Keyboard event failed");
}
inline void brief(std::chrono::milliseconds delay) {
    const auto until_time = std::chrono::steady_clock::now() + delay;
    until([&] { return std::chrono::steady_clock::now() >= until_time; }, delay + 300ms);
}
inline POINT center(RECT r) { return {(r.left + r.right) / 2, (r.top + r.bottom) / 2}; }
inline POINT single_rope_point(const Desktop& host) {
    POINT point{};
    require(until([&] {
        const auto rows = host.frame().rows;
        if (rows.size() != 1 || !IsWindowVisible(host.workspace)) return false;
        point = center(rows.front().second);
        return true;
    }), "Single rope geometry unavailable");
    return point;
}
inline void aim(HWND surface, POINT p) {
    // A native popup can be visible before Windows starts routing input to it.
    if (!until([&] { return WindowFromPoint(p) == surface; }, 1500ms)) {
        wchar_t wanted[96]{}, hit[96]{};
        GetClassNameW(surface, wanted, 96);
        GetClassNameW(WindowFromPoint(p), hit, 96);
        std::wcerr << L"Expected " << wanted << L", hit " << hit << L" at " << p.x << L"," << p.y << L'\n';
        throw std::runtime_error("Test target is covered");
    }
    require(SetCursorPos(p.x, p.y) != FALSE, "Could not address card");
}
inline void show(Desktop& host, const Fields& fields) {
    if (IsWindowVisible(host.workspace)) return;
    require(host.contains("hotkey registered") && GetForegroundWindow() == fields.window,
            "Test hotkey or foreground unavailable");
    key(VK_CONTROL, true); key(VK_MENU, true); key(VK_F11, true);
    key(VK_F11, false); key(VK_MENU, false); key(VK_CONTROL, false);
    require(until([&] { return IsWindowVisible(host.workspace) != 0; }), "Global hotkey did not show the workspace");
}
} // namespace workspace_fixture
