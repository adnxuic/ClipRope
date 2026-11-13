#pragma once

#include "../integration/clipboard_process.hpp"
#include <ole2.h>
#include <functional>
#include <stdexcept>

namespace probe {
inline void require(bool ok, const char* label) {
    if (!ok) throw std::runtime_error(label);
}

inline void pump_until(const std::function<bool()>& condition, const char* label, DWORD timeout_ms = 10000) {
    const auto deadline = GetTickCount64() + timeout_ms;
    while (!condition()) {
        require(GetTickCount64() < deadline, label);
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

class ExternalCopy {
public:
    ExternalCopy(const wchar_t* producer, const wchar_t* mode) {
        static unsigned counter{};
        auto key = L"Local\\ClipRopeProbe-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++counter);
        auto event = [&key](const wchar_t* suffix) {
            auto handle = CreateEventW(nullptr, TRUE, FALSE, (key + suffix).c_str());
            require(handle != nullptr, "CreateEvent failed");
            return handle;
        };
        ready_ = event(L"-ready");
        reading = event(L"-reading");
        proceed = event(L"-proceed");
        stop_ = event(L"-stop");
        process_ = probe::launch({producer, mode, key}, probe::desktop_path());
        require(process_ != nullptr, "External producer launch failed");
        pump_until([&] {
            require(WaitForSingleObject(process_, 0) == WAIT_TIMEOUT, "External producer exited before publication");
            return WaitForSingleObject(ready_, 0) == WAIT_OBJECT_0;
        }, "External producer did not publish", std::wstring_view(mode) == L"after-resume" ? 125000 : 10000);
    }
    ~ExternalCopy() {
        SetEvent(proceed);
        SetEvent(stop_);
        DWORD index{};
        CoWaitForMultipleHandles(COWAIT_DISPATCH_WINDOW_MESSAGES | COWAIT_DISPATCH_CALLS, 4000, 1, &process_, &index);
        for (auto handle : {ready_, reading, proceed, stop_, process_}) CloseHandle(handle);
    }
    HANDLE reading{}, proceed{};
private:
    HANDLE ready_{}, stop_{}, process_{};
};

} // namespace probe
