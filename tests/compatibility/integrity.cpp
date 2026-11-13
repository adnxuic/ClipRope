#include "desktop_fixture.hpp"
#include "platform_win/delivery.hpp"
#include <sddl.h>
#include <memory>
#include <iostream>

namespace {
using desktop_fixture::require;
using desktop_fixture::Fields;
using cliprope::win::DeliveryResult;
struct Close { void operator()(void* value) const { CloseHandle(value); } };
using Handle = std::unique_ptr<void, Close>;

Handle token(HANDLE process, DWORD access) {
    HANDLE raw{};
    require(OpenProcessToken(process, access, &raw) != FALSE, "Fixture process token unavailable");
    return Handle(raw);
}
DWORD integrity(HANDLE process) {
    auto target = token(process, TOKEN_QUERY);
    DWORD n{};
    GetTokenInformation(target.get(), TokenIntegrityLevel, nullptr, 0, &n);
    std::vector<BYTE> buffer(n);
    require(n && GetTokenInformation(target.get(), TokenIntegrityLevel, buffer.data(), n, &n), "Token label unavailable");
    auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data())->Label.Sid;
    require(IsValidSid(sid) && *GetSidSubAuthorityCount(sid), "Token label invalid");
    return *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
}
Handle launch_low(const wchar_t* exe, HWND window, DWORD pid) {
    auto own = token(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY | TOKEN_QUERY | TOKEN_ADJUST_DEFAULT);
    HANDLE raw{};
    require(CreateRestrictedToken(own.get(), DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 0, nullptr, &raw) != FALSE,
            "Restricted primary token unavailable");
    Handle restricted(raw);
    PSID low{};
    require(ConvertStringSidToSidW(L"S-1-16-4096", &low) != FALSE, "Low integrity SID unavailable");
    TOKEN_MANDATORY_LABEL label{{low, SE_GROUP_INTEGRITY}};
    const bool lowered = SetTokenInformation(restricted.get(), TokenIntegrityLevel, &label,
                                             sizeof(label) + GetLengthSid(low)) != FALSE;
    if (!lowered) std::cerr << "SetTokenInformation error=" << GetLastError() << '\n';
    LocalFree(low);
    require(lowered, "Could not assign Low integrity to the disposable caller");
    auto command = probe::quote(exe) + L" --low-caller " + std::to_wstring(reinterpret_cast<UINT_PTR>(window)) +
                   L" " + std::to_wstring(pid);
    auto desk = probe::desktop_path();
    STARTUPINFOW startup{sizeof(startup)};
    startup.lpDesktop = desk.data();
    PROCESS_INFORMATION child{};
    if (!CreateProcessAsUserW(restricted.get(), exe, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                              nullptr, nullptr, &startup, &child)) {
        std::cerr << "CreateProcessAsUser error=" << GetLastError() << '\n';
        throw std::runtime_error("Low-integrity process launch refused");
    }
    CloseHandle(child.hThread);
    return Handle(child.hProcess);
}
int lower_caller(HWND expected, DWORD expected_pid) {
    if (integrity(GetCurrentProcess()) != SECURITY_MANDATORY_LOW_RID) return 3;
    DWORD pid{};
    GetWindowThreadProcessId(expected, &pid);
    wchar_t name[64]{};
    if (pid != expected_pid || !GetClassNameW(expected, name, 64) || wcscmp(name, fixture::class_name) ||
        GetForegroundWindow() != expected) return 4;
    GUITHREADINFO gui{sizeof(gui)};
    if (!GetGUIThreadInfo(0, &gui) || gui.hwndActive != expected || GetDlgCtrlID(gui.hwndFocus) != 1 ||
        GetAncestor(gui.hwndFocus, GA_ROOT) != expected) return 5;
    // Exercise the production delivery gate independently of clipboard writes.
    cliprope::win::EditGesture gesture;
    gesture.bind();
    if (gesture.deliver([] { return true; }) != DeliveryResult::access_denied) return 6;
    return gesture.deliver([] { return true; }) == DeliveryResult::consumed ? 0 : 7;
}
void exercise() {
    struct Cursor {
        POINT previous{};
        bool saved{GetCursorPos(&previous) != FALSE};
        ~Cursor() { if (saved) SetCursorPos(previous.x, previous.y); }
    } cursor;
    Fields fields;
    fields.start();
    desktop_fixture::reset_fields(fields);
    const auto target = integrity(fields.process);
    require(target == SECURITY_MANDATORY_MEDIUM_RID, "Expected a standard Medium integrity target process");
    std::wstring exe(32768, L'\0');
    const auto n = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    require(n && n < exe.size(), "Cannot locate fixture caller");
    exe.resize(n);
    auto low = launch_low(exe.c_str(), fields.window, fields.pid);
    require(integrity(low.get()) == SECURITY_MANDATORY_LOW_RID, "Child lacks a Low primary token");
    require(WaitForSingleObject(low.get(), 5000) == WAIT_OBJECT_0, "Low caller timed out");
    DWORD code{};
    require(GetExitCodeProcess(low.get(), &code) != FALSE, "Low caller exit unavailable");
    if (code != 0) std::cerr << "Low caller check=" << code << '\n';
    require(code == 0, "Production delivery did not return one higher-integrity denial");
    require(fields.get(fixture::Query::edit_attempts) == 0 && fields.get(fixture::Query::changes_a) == 0 &&
            fields.get(fixture::Query::changes_b) == 0 && fields.get(fixture::Query::a_unchanged) &&
            fields.get(fixture::Query::b_unchanged), "Higher-integrity fields changed");
    std::cout << "caller-il=4096 target-il=" << target << " selected=A delivery=access-denied repeated=consumed "
                 "attempts=0 changes-A=0 changes-B=0\n";
}
}
int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        if (argc == 3 && std::wstring_view(argv[1]) == L"--fields")
            return fixture::run_fields(std::stoul(argv[2]));
        if (argc == 4 && std::wstring_view(argv[1]) == L"--low-caller")
            return lower_caller(reinterpret_cast<HWND>(std::stoull(argv[2])), std::stoul(argv[3]));
        if (argc != 2 || std::wstring_view(argv[1]) != L"--input-in-disposable-session") return 2;
        exercise();
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
