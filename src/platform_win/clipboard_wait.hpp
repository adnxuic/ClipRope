#pragma once
#include <windows.h>
#include <objbase.h>

namespace cliprope {
// A reader can hold OpenClipboard while waiting for WM_RENDERFORMAT or COM
// from the publication apartment. A sleeping owner starves that reader.
inline void pump_clipboard_reader(DWORD milliseconds) {
    HRESULT wait = E_FAIL;
    if (auto event = CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        DWORD index{};
        wait = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS | COWAIT_DISPATCH_WINDOW_MESSAGES, milliseconds, 1, &event, &index);
        CloseHandle(event);
    }
    if (FAILED(wait) && wait != RPC_S_CALLPENDING) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, milliseconds, QS_SENDMESSAGE);
    }
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, WM_RENDERFORMAT, WM_RENDERALLFORMATS, PM_REMOVE))
        DispatchMessageW(&msg);
}
} // namespace cliprope
