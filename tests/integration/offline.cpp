#include <winsock2.h>
#include <ws2tcpip.h>
#include <psapi.h>
#include <map>
#include <iostream>
#include <algorithm>
#include "capture_worker/capture.hpp"
#include "../clipboard_sources/external_copy.hpp"
#include "../clipboard_sources/sample.hpp"
#include "history_fixture.hpp"

namespace {
using probe::require;
namespace fs = std::filesystem;
constexpr DWORD socket_denied = 79;
struct Trace {
    unsigned processes{}, workers{}, denied{};
    DWORD status{};
};

// Stop only descendants launched by this fixture. A loader notification arrives
// before the DLL entry point runs. Vetoing WinSock prevents both IPv4 and IPv6
// socket creation, including dynamically resolved calls.
Trace run(const fs::path& exe, const std::vector<std::wstring>& args) {
    require(probe::private_station(), "Network guard requires a private station");
    std::wstring command = probe::quote(exe.wstring());
    for (const auto& arg : args) command += L" " + probe::quote(arg);
    auto desktop = probe::desktop_path();
    STARTUPINFOW startup{sizeof(startup)};
    startup.lpDesktop = desktop.data();
    PROCESS_INFORMATION child{};
    require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
                           DEBUG_PROCESS | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child) != FALSE,
            "Could not start disposable debug tree");
    // CREATE_PROCESS_DEBUG_EVENT supplies handles held by Windows during tracing.
    CloseHandle(child.hThread);
    CloseHandle(child.hProcess);
    std::map<DWORD, HANDLE> tracked;
    Trace trace;
    const auto deadline = GetTickCount64() + 15000;
    do {
        DEBUG_EVENT event{};
        require(GetTickCount64() < deadline && WaitForDebugEvent(&event, 8000), "Disposable process tree timed out");
        const auto pid = event.dwProcessId;
        DWORD continuation = DBG_CONTINUE;
        switch (event.dwDebugEventCode) {
        case CREATE_PROCESS_DEBUG_EVENT: {
            tracked.emplace(pid, event.u.CreateProcessInfo.hProcess);
            if (event.u.CreateProcessInfo.hFile) CloseHandle(event.u.CreateProcessInfo.hFile);
            wchar_t name[32768]{};
            DWORD length = 32768;
            require(QueryFullProcessImageNameW(tracked.at(pid), 0, name, &length) != FALSE, "Cannot verify process identity");
            const auto basename = fs::path(name).filename().wstring();
            trace.workers += _wcsicmp(basename.c_str(), L"ClipRopeWorker.exe") == 0;
            std::wcout << L"verified-child=" << basename << L'\n';
            ++trace.processes;
            break;
        }
        case LOAD_DLL_DEBUG_EVENT: {
            auto file = event.u.LoadDll.hFile;
            wchar_t path[32768]{};
            bool named = file && GetFinalPathNameByHandleW(file, path, 32768, FILE_NAME_NORMALIZED);
            if (file) CloseHandle(file);
            if (!named) named = GetMappedFileNameW(tracked.at(pid), event.u.LoadDll.lpBaseOfDll, path, 32768) != 0;
            require(named, "Cannot verify a loaded module");
            const auto name = fs::path(path).filename().wstring();
            if (!_wcsicmp(name.c_str(), L"ws2_32.dll") || !_wcsicmp(name.c_str(), L"wsock32.dll")) {
                ++trace.denied;
                require(TerminateProcess(tracked.at(pid), socket_denied) != FALSE, "Socket module could not be vetoed");
            }
            break;
        }
        case EXCEPTION_DEBUG_EVENT:
            if (event.u.Exception.ExceptionRecord.ExceptionCode != EXCEPTION_BREAKPOINT)
                continuation = DBG_EXCEPTION_NOT_HANDLED;
            break;
        case EXIT_PROCESS_DEBUG_EVENT:
            tracked.erase(pid);
            if (pid == child.dwProcessId) trace.status = event.u.ExitProcess.dwExitCode;
            break;
        }
        require(ContinueDebugEvent(pid, event.dwThreadId, continuation) != FALSE, "Could not resume disposable process");
    } while (!tracked.empty());
    return trace;
}
struct Server {
    SOCKET socket{INVALID_SOCKET};
    unsigned port{};
    Server() {
        socket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in localhost{};
        localhost.sin_family = AF_INET;
        localhost.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        int size = sizeof(localhost);
        require(socket != INVALID_SOCKET && !bind(socket, reinterpret_cast<sockaddr*>(&localhost), size) &&
                !listen(socket, 4) && !getsockname(socket, reinterpret_cast<sockaddr*>(&localhost), &size),
                "Local sentinel unavailable");
        port = ntohs(localhost.sin_port);
    }
    ~Server() { closesocket(socket); }
};
int network_probe(unsigned port) {
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dest.sin_port = htons(static_cast<USHORT>(port));
    const int result = client == INVALID_SOCKET ? SOCKET_ERROR :
        connect(client, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    const int error = result == SOCKET_ERROR ? WSAGetLastError() : 0;
    if (client != INVALID_SOCKET) closesocket(client);
    return error;
}
void exercise(const fs::path& headless, const fs::path& source) {
    require(probe::private_station(), "Offline test requires private station");
    history_fixture::Directory dir;
    wchar_t self[32768]{};
    require(GetModuleFileNameW(nullptr, self, 32768) != 0, "Cannot locate canary");
    Server local;
    require(network_probe(local.port) == 0, "Control failed to reach local sentinel");
    const auto check = run(self, {L"--canary", std::to_wstring(local.port)});
    require(check.status == socket_denied && check.denied == 1, "Socket canary was not vetoed");
    {
        probe::ExternalCopy first(source.c_str(), L"normal");
        const auto captured = run(headless, {dir.path.wstring(), L"capture"});
        std::cout << "capture-tree=" << captured.processes << " status=" << captured.status << " blocked=" << captured.denied << std::endl;
        require(captured.workers == 1 && !captured.status && !captured.denied, "Production capture failed under network veto");
        probe::ExternalCopy overwrite(source.c_str(), L"second");
        require(cliprope::capture_clipboard(GetClipboardSequenceNumber()).saved == sample::formats(true), "Overwrite failed");
    }
    const auto recalled = run(headless, {dir.path.wstring(), L"head"});
    require(!recalled.workers && !recalled.status && !recalled.denied, "Production recall failed under network veto");
    const auto sequence = GetClipboardSequenceNumber();
    const auto restored = cliprope::capture_clipboard(sequence).saved, expected = sample::formats(false);
    require(std::is_permutation(restored.begin(), restored.end(), expected.begin(), expected.end()), "Offline payload mismatch");
    require(OpenClipboard(nullptr) != FALSE, "Cannot clean up proven fixture data");
    const bool clean = GetClipboardSequenceNumber() == sequence && EmptyClipboard();
    CloseClipboard();
    require(clean, "Clipboard changed before cleanup");
    std::cout << "socket-canary=vetoed capture-worker=production restore=Unicode+HTML external-producer=exited\n";
}
}
int wmain(int argc, wchar_t** argv) {
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data)) return 1;
    if (argc == 3 && std::wstring_view(argv[1]) == L"--canary") return network_probe(std::stoul(argv[2]));
    if (argc != 3 || FAILED(OleInitialize(nullptr))) return 2;
    int status{};
    try { exercise(argv[1], argv[2]); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; status = 1; }
    OleUninitialize();
    WSACleanup();
    return status;
}
