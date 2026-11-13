#pragma once
#include "../clipboard_sources/external_copy.hpp"
#include <filesystem>
#include <optional>

namespace probe {
inline HWND owned_window(DWORD pid, const wchar_t* class_name) {
    HWND found{};
    while ((found = FindWindowExW(nullptr, found, class_name, nullptr)) != nullptr) {
        DWORD owner{};
        GetWindowThreadProcessId(found, &owner);
        if (owner == pid) return found;
    }
    return nullptr;
}

class Resident {
public:
    Resident(const wchar_t* exe, const std::filesystem::path& profile, bool show,
             const std::filesystem::path& receipt = {}, const wchar_t* fault = nullptr,
             std::optional<bool> expected_visible = {}, bool export_diagnostics = false) {
        std::vector<std::wstring> args{exe, L"--profile", profile.wstring()};
        if (show) args.push_back(L"--show");
        if (export_diagnostics) args.push_back(L"--export-diagnostics-on-exit");
        if (!receipt.empty()) {
            args.push_back(L"--probe");
            args.push_back(receipt.wstring());
        }
        if (fault) { args.push_back(L"--fault"); args.push_back(fault); }
        process_ = probe::launch(args, probe::desktop_path());
        require(process_ != nullptr, "Desktop host launch failed");
        try {
            probe::pump_until([&] {
                require(WaitForSingleObject(process_, 0) == WAIT_TIMEOUT, "Desktop host exited during startup");
                listener_ = owned_window(GetProcessId(process_), L"ClipRope.Listener");
                wchar_t title[16]{};
                if (listener_) GetWindowTextW(listener_, title, 16);
                return std::wstring_view(title) == L"ClipRope";
            }, "Listener did not become ready");
            workspace = owned_window(GetProcessId(process_), L"ClipRope.Workspace");
            std::cout << "startup expected-visible=" << expected_visible.value_or(show) << " workspace-found=" << (workspace != nullptr)
                      << " WS_VISIBLE=" << ((GetWindowLongPtrW(workspace, GWL_STYLE) & WS_VISIBLE) != 0)
                      << " native-visible=" << (IsWindowVisible(workspace) != FALSE) << '\n';
            // A noninteractive station has no displayed desktop. USER still
            // records the application's own show/hide state in the window style.
            require(workspace && ((GetWindowLongPtrW(workspace, GWL_STYLE) & WS_VISIBLE) != 0) == expected_visible.value_or(show),
                    "Unexpected startup visibility");
        } catch (...) { close(); throw; }
    }
    ~Resident() { close(); }
    HANDLE handle() const { return process_; }
    HWND listener() const { return listener_; }
    DWORD pid() const { return GetProcessId(process_); }
    void exit() {
        require(PostMessageW(listener_, WM_CLOSE, 0, 0) != FALSE, "Desktop shutdown request failed");
        probe::pump_until([&] { return WaitForSingleObject(process_, 0) == WAIT_OBJECT_0; }, "Desktop host did not exit");
        DWORD status{};
        require(GetExitCodeProcess(process_, &status) && status == 0, "Desktop host reported an error");
    }
    HWND workspace{};
private:
    void close() {
        if (process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
            if (listener_) PostMessageW(listener_, WM_CLOSE, 0, 0);
            if (WaitForSingleObject(process_, 3000) == WAIT_TIMEOUT) {
                TerminateProcess(process_, 1);
                WaitForSingleObject(process_, 3000);
            }
        }
        if (process_) CloseHandle(process_);
        process_ = nullptr;
    }
    HANDLE process_{};
    HWND listener_{};
};

} // namespace probe
