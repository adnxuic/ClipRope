#include <winsock2.h>
#include "preview_worker/protocol.hpp"
#include <shellapi.h>
#include <array>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace cliprope::preview_worker;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::unique_ptr<void, decltype(&LocalFree)> args(argv, LocalFree);
    if (!argv || argc != 4)
        return 2;
    try {
        Handle input(reinterpret_cast<HANDLE>(std::stoull(argv[1]))),
            output(reinterpret_cast<HANDLE>(std::stoull(argv[2])));
        const auto length = std::stoull(argv[3]);
        Mapping in(input.value, FILE_MAP_READ, sizeof(Request) + size_t(length)),
            out(output.value, FILE_MAP_WRITE, output_size);
        if (!in.data || !out.data)
            return 3;
        Request request{};
        std::memcpy(&request, in.data, sizeof request);
        if (request.serial == 201) {
            Sleep(10000);
            return 1;
        }
        if (request.serial == 202) {
            RaiseFailFastException(nullptr, nullptr, 0);
            return 1;
        }
        Response response;
        response.serial = request.serial;
        response.status = 0;
        response.width = response.height = response.source_width = response.source_height = 2;
        response.stride = 8;
        response.length = 16;
        std::array<unsigned char, 16> pixels{7, 0, 0, 255, 7, 0, 0, 255,
                                             7, 0, 0, 255, 7, 0, 0, 255};
        if (request.serial == 203)
            response.length = 0xffffffff;
        if (request.serial == 205)
            ++response.serial;
        if (request.serial == 206)
            ++response.protocol_version;
        if (request.serial == 207)
            response.stride = 0xffffffff;
        if (request.serial == 208)
            response.source_width = 3;
        if (request.serial == 204) {
            if (length < 56 + 2)
                return 4;
            std::wstring path;
            const auto *bytes = static_cast<const unsigned char *>(in.data) + sizeof(Request) + 56;
            for (size_t n = 0; n + 1 < length - 56; n += 2) {
                auto c = wchar_t(bytes[n] | (bytes[n + 1] << 8));
                if (!c)
                    break;
                path += c;
            }
            Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, 0, nullptr));
            bool denied_file =
                file.value == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED;
            WSADATA data{};
            bool denied_network = false;
            if (!WSAStartup(MAKEWORD(2, 2), &data)) {
                auto socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                u_long nonblocking = 1;
                ioctlsocket(socket_handle, FIONBIO, &nonblocking);
                sockaddr_in address{};
                address.sin_family = AF_INET;
                const auto offset = (path.size() + 1) * 2;
                if (offset + 2 > length - 56)
                    return 4;
                address.sin_port =
                    htons(static_cast<u_short>(bytes[offset] | (bytes[offset + 1] << 8)));
                address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                const auto connected =
                    connect(socket_handle, reinterpret_cast<sockaddr *>(&address), sizeof address);
                const auto error = WSAGetLastError();
                denied_network = connected == SOCKET_ERROR && error == WSAEACCES;
                if (connected == SOCKET_ERROR && error == WSAEWOULDBLOCK) {
                    fd_set write{}, failed{};
                    FD_ZERO(&write);
                    FD_ZERO(&failed);
                    FD_SET(socket_handle, &write);
                    FD_SET(socket_handle, &failed);
                    timeval timeout{0, 250000};
                    const auto ready = select(0, nullptr, &write, &failed, &timeout);
                    int result{};
                    int result_size = sizeof result;
                    getsockopt(socket_handle, SOL_SOCKET, SO_ERROR,
                               reinterpret_cast<char *>(&result), &result_size);
                    denied_network = ready == 0 || (ready > 0 && result == WSAEACCES);
                }
                closesocket(socket_handle);
                WSACleanup();
            }
            wchar_t exe[32768]{};
            GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
            STARTUPINFOW startup{sizeof startup};
            PROCESS_INFORMATION process{};
            bool denied_child =
                !CreateProcessW(exe, nullptr, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                                nullptr, &startup, &process);
            if (!denied_child) {
                TerminateProcess(process.hProcess, 1);
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
            pixels[0] = static_cast<unsigned char>(
                (denied_file ? 1 : 0) | (denied_network ? 2 : 0) | (denied_child ? 4 : 0));
        }
        std::memcpy(out.data, &response, sizeof response);
        std::memcpy(static_cast<unsigned char *>(out.data) + sizeof response, pixels.data(),
                    pixels.size());
        return 0;
    } catch (...) {
        return 5;
    }
}
