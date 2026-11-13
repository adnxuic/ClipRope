#include "delivery_fixture.hpp"
#include "../clipboard_sources/sample.hpp"

#include <array>
#include <richedit.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include "../clipboard_sources/media_sample.hpp"
#include <cstring>
#include <string_view>
#include <vector>

namespace fixture {
namespace {
HWND first{}, second{}, button{};
HWND ansi_field{};
WNDPROC default_edit{}, default_ansi{};
unsigned ansi_png_pastes{};
HWND attachment_field{};
WNDPROC attachment_default{};
unsigned attachments{};

LRESULT CALLBACK attachment_proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    // The host handles the shortcut independently of the control's built-in paste command.
    if (msg == EM_CANPASTE && w == CF_HDROP) return FALSE;
    if (msg == WM_KEYDOWN && w == 'V' && GetKeyState(VK_CONTROL) < 0 && OpenClipboard(window)) {
        auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
        wchar_t name[MAX_PATH]{};
        if (drop && DragQueryFileW(drop, 0xffffffff, nullptr, 0) == 1 && DragQueryFileW(drop, 0, name, MAX_PATH)) {
            std::filesystem::path path(name);
            if (path.filename() == L"fixture-picture.png") {
                std::ifstream in(path, std::ios::binary);
                const auto expected = media_sample::png().bytes;
                std::vector<unsigned char> sample(expected.size());
                if (in.read(reinterpret_cast<char*>(sample.data()), sample.size()) && sample == expected)
                    ++attachments;
            }
        }
        CloseClipboard();
        return 0;
    }
    return CallWindowProcW(attachment_default, window, msg, w, l);
}
unsigned first_changes{}, second_changes{}, attempts{};
Action fault{};
HANDLE parent{}, self{};
std::vector<BYTE> original_dacl;

LRESULT CALLBACK first_proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CHAR && w == 22) {
        ++attempts;
        if (fault == Action::suppress_after || fault == Action::suppress_without) {
            if (fault == Action::suppress_after) CallWindowProcW(default_edit, window, msg, w, l);
            // A consumer may accept or discard the shortcut after dispatch.
            return 0;
        }
    }
    return CallWindowProcW(default_edit, window, msg, w, l);
}

LRESULT CALLBACK ansi_proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_CHAR && w == 22 && IsClipboardFormatAvailable(RegisterClipboardFormatW(L"PNG"))) {
        ++ansi_png_pastes;
        return 0;
    }
    return CallWindowProcA(default_ansi, window, msg, w, l);
}

bool deny_query() {
    DWORD needed{};
    GetKernelObjectSecurity(self, DACL_SECURITY_INFORMATION, nullptr, 0, &needed);
    original_dacl.resize(needed);
    if (!needed || !GetKernelObjectSecurity(self, DACL_SECURITY_INFORMATION, original_dacl.data(), needed, &needed)) return false;
    SECURITY_DESCRIPTOR sd{};
    ACL empty{};
    return InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION) &&
           InitializeAcl(&empty, sizeof(empty), ACL_REVISION) &&
           SetSecurityDescriptorDacl(&sd, TRUE, &empty, FALSE) &&
           SetKernelObjectSecurity(self, DACL_SECURITY_INFORMATION, &sd);
}

bool overwrite(HWND owner) {
    constexpr wchar_t replacement[] = L"new copy";
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, sizeof(replacement));
    void* data = mem ? GlobalLock(mem) : nullptr;
    if (!data) { if (mem) GlobalFree(mem); return false; }
    std::memcpy(data, replacement, sizeof(replacement));
    GlobalUnlock(mem);
    bool ok = false;
    if (OpenClipboard(owner)) {
        ok = EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, mem);
        CloseClipboard();
    }
    if (!ok) GlobalFree(mem);
    return ok;
}

bool act(HWND window, Action action) {
    switch (action) {
    case Action::reset: {
        if (!original_dacl.empty()) {
            if (!SetKernelObjectSecurity(self, DACL_SECURITY_INFORMATION, original_dacl.data())) return false;
            original_dacl.clear();
        }
        if (!first) {
            first = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                  12, 15, 210, 28, window, reinterpret_cast<HMENU>(1), nullptr, nullptr);
            if (!first) return false;
            default_edit = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(first, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(first_proc)));
            if (!default_edit) return false;
        }
        SendMessageW(first, EM_SETREADONLY, FALSE, 0);
        SetWindowTextW(first, L"left|right");
        SetWindowTextW(second, L"untouched");
        fault = Action::reset;
        SetForegroundWindow(window);
        SetFocus(first);
        SendMessageW(first, EM_SETSEL, 4, 5);
        first_changes = second_changes = attempts = 0;
        return GetFocus() == first;
    }
    case Action::caret: SendMessageW(first, EM_SETSEL, 4, 4); return GetFocus() == first;
    case Action::focus_button: SetFocus(button); return GetFocus() == button;
    case Action::focus_b:
        SetFocus(second);
        SendMessageW(second, EM_SETSEL, 0, -1);
        return GetFocus() == second;
    case Action::destroy_a: if (!DestroyWindow(first)) return false; first = nullptr; return true;
    case Action::readonly_a: return SendMessageW(first, EM_SETREADONLY, TRUE, 0) != 0;
    case Action::deny_query: return deny_query();
    case Action::overwrite: return overwrite(window);
    case Action::suppress_after:
    case Action::suppress_without: fault = action; return true;
    case Action::focus_attachment:
        if (!attachment_field) {
            if (!LoadLibraryW(L"Msftedit.dll")) return false;
            attachment_field = CreateWindowW(L"RICHEDIT50W", L"", WS_CHILD | WS_VISIBLE, 12, 135, 180, 20,
                                             window, nullptr, nullptr, nullptr);
            if (!attachment_field) return false;
            attachment_default = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(attachment_field, GWLP_WNDPROC,
                                                     reinterpret_cast<LONG_PTR>(attachment_proc)));
        }
        attachments = 0;
        SetFocus(attachment_field);
        return attachment_default && GetFocus() == attachment_field;
    case Action::focus_ansi:
        if (!ansi_field) {
            ansi_field = CreateWindowA("EDIT", "", WS_CHILD | WS_VISIBLE, 12, 135, 180, 20,
                                       window, nullptr, nullptr, nullptr);
            if (!ansi_field) return false;
            default_ansi = reinterpret_cast<WNDPROC>(SetWindowLongPtrA(ansi_field, GWLP_WNDPROC,
                                                        reinterpret_cast<LONG_PTR>(ansi_proc)));
        }
        ansi_png_pastes = 0;
        SetFocus(ansi_field);
        return default_ansi && !IsWindowUnicode(ansi_field) && GetFocus() == ansi_field;
    case Action::paste_b:
        if (GetFocus() != second) return false;
        SendMessageW(second, WM_CHAR, 22, 0); // Classic EDIT's Ctrl+V character.
        return true;
    }
    return false;
}

bool equals(HWND field, std::wstring_view expected) {
    wchar_t buffer[128]{};
    return field && GetWindowTextW(field, buffer, static_cast<int>(std::size(buffer))) &&
           std::wstring_view(buffer) == expected;
}

LRESULT CALLBACK window_proc(HWND window, UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_CREATE:
        second = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                               12, 56, 210, 28, window, reinterpret_cast<HMENU>(2), nullptr, nullptr);
        button = CreateWindowW(L"BUTTON", L"No editable target", WS_CHILD | WS_VISIBLE,
                               12, 100, 210, 30, window, nullptr, nullptr, nullptr);
        SetTimer(window, 1, 500, nullptr);
        return second && button ? 0 : -1;
    case WM_TIMER:
        if (WaitForSingleObject(parent, 0) != WAIT_TIMEOUT) DestroyWindow(window);
        return 0;
    case WM_COMMAND:
        if (HIWORD(w) == EN_CHANGE && reinterpret_cast<HWND>(l) == first) ++first_changes;
        if (HIWORD(w) == EN_CHANGE && reinterpret_cast<HWND>(l) == second) ++second_changes;
        return 0;
    case command: return act(window, static_cast<Action>(w));
    case inspect:
        switch (static_cast<Query>(w)) {
        case Query::changes_a: return first_changes;
        case Query::changes_b: return second_changes;
        case Query::edit_attempts: return attempts;
        case Query::attachments: return attachments;
        case Query::ansi_png_pastes: return ansi_png_pastes;
        case Query::a_unchanged: return equals(first, L"left|right");
        case Query::a_without_selection: return equals(first, L"leftright");
        case Query::a_replaced: return equals(first, L"left" + sample::text(false) + L"right");
        case Query::a_inserted: return equals(first, L"left" + sample::text(false) + L"|right");
        case Query::b_unchanged: return equals(second, L"untouched");
        case Query::b_pasted: return equals(second, sample::text(false));
        case Query::b_pasted_new: return equals(second, L"new copy");
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(window, msg, w, l);
}
} // namespace

int run_fields(DWORD parent_pid) {
    parent = OpenProcess(SYNCHRONIZE, FALSE, parent_pid);
    self = OpenProcess(READ_CONTROL | WRITE_DAC, FALSE, GetCurrentProcessId());
    if (!parent || !self) return 1;
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.lpszClassName = class_name;
    if (!RegisterClassW(&wc)) return 1;
    HWND window = CreateWindowW(class_name, L"ClipRope probe fields", WS_OVERLAPPEDWINDOW,
                                32, 32, 292, 202, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window) return 1;
    ShowWindow(window, SW_SHOW);
    MSG msg{};
    BOOL got{};
    while ((got = GetMessageW(&msg, nullptr, 0, 0)) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CloseHandle(parent);
    CloseHandle(self);
    return got < 0 ? 1 : 0;
}
} // namespace fixture
