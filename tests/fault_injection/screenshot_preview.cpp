#include <winsock2.h>
#include "preview_worker/process.hpp"
#include "rendering/screenshot_preview_service.hpp"
#include "rendering/thumbnail_cache.hpp"
#include "../clipboard_sources/media_sample.hpp"
#include "../integration/history_fixture.hpp"
#include <fstream>
#include <iostream>
#include <thread>
#include <atomic>
#include <utility>

using namespace cliprope;
using namespace history_fixture;
namespace {
using Clock = std::chrono::steady_clock;

void shutdown_cases(const std::filesystem::path& path) {
    auto sample = media_sample::dib(false);
    preview_worker::Request request;
    request.encoding = application::ScreenshotEncoding::dib;
    request.width = request.height = 128;
    request.length = static_cast<unsigned>(sample.bytes.size());
    auto decode = [&](std::stop_token stop = {}) {
        return preview_worker::decode(request, sample.bytes, stop, Clock::now() + std::chrono::seconds(2), path);
    };
    for (bool deny : {true, false}) {
        preview_worker::StopProbe probe;
        probe.deny_termination = deny;
        probe.fail_wait = !deny;
        request.serial = 201; // Worker waits for ten seconds until the Job terminates it.
        preview_worker::observe_stop(&probe);
        const auto started = Clock::now();
        try { decode(probe.cancel.get_token()); throw std::runtime_error("Forced shutdown failure succeeded"); }
        catch (const preview_worker::ShutdownFailure& error) {
            require(error.termination_error == (deny ? ERROR_ACCESS_DENIED : 0) &&
                    error.wait_error == (deny ? ERROR_TIMEOUT : ERROR_INVALID_HANDLE) &&
                    (!deny || !error.confirmed_exit), "Wrong native shutdown diagnosis");
        }
        preview_worker::observe_stop(nullptr);
        const auto duration = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
        require(duration < 1800, "Preview shutdown did not return a bounded failure");
        if (deny) {
            require(WaitForSingleObject(probe.child.value, 0) == WAIT_TIMEOUT, "Missing late child");
            request.serial = 210;
            try { decode(); throw std::runtime_error("Overlapping replacement was launched"); }
            catch (const preview_worker::ShutdownFailure& error) {
                require(error.wait_error == ERROR_BUSY && !error.confirmed_exit, "Missing outstanding child diagnosis");
            }
        }
        CloseHandle(std::exchange(probe.keep_job.value, nullptr));
        require(WaitForSingleObject(probe.child.value, 2000) == WAIT_OBJECT_0, "Kill-on-close failed to reap child");
        const auto reap_deadline = Clock::now() + std::chrono::seconds(2);
        while (preview_worker::holding_late_child() && Clock::now() < reap_deadline) Sleep(2);
        require(!preview_worker::holding_late_child(), "Idle preview retained a signaled process handle");
        request.serial = 210;
        require(decode().width == 2, "Late exit did not unblock a fresh request");
        std::cout << "termination_denied=" << deny << " shutdown_ms=" << duration << " replacement=after-exit\n";
    }
    request.serial = 201;
    try { decode(); throw std::runtime_error("Timed-out worker returned pixels"); }
    catch (const preview_worker::SandboxFailure& e) {
        require(e.native_code == ERROR_TIMEOUT, "Deadline returned the wrong reason");
    }
}

void owner_shutdown() {
    HWND window = CreateWindowW(L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    require(window != nullptr, "Callback owner unavailable");
    preview_worker::Handle reading(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    require(reading.value != nullptr, "Resolve barrier unavailable");
    std::atomic<unsigned> posted{};
    auto service = std::make_unique<rendering::ScreenshotPreviewService>(
        [&](const auto&, auto stop, auto deadline) -> application::ScreenshotSource {
            SetEvent(reading.value);
            while (!application::preview_expired(stop, deadline)) Sleep(1);
            throw std::runtime_error("Cancelled fixture read");
        }, [&] { ++posted; PostMessageW(window, WM_APP + 6, 0, 0); });
    application::ScreenshotPreviewRequest request{};
    request.serial = 1;
    service->request(request);
    require(WaitForSingleObject(reading.value, 2000) == WAIT_OBJECT_0, "Pending read did not start");
    service->cancel(true);
    DestroyWindow(window);
    const auto start = Clock::now();
    service.reset();
    require(!posted && Clock::now() - start < std::chrono::seconds(1), "Late notification reached retired owner");
}

void measure_thumbnail_shutdown() {
    Directory dir;
    auto cache = std::make_unique<rendering::ThumbnailCache>(dir.path);
    cache->request(std::string(64, '1'), CF_DIB);
    const auto start = Clock::now();
    cache.reset();
    const auto elapsed = Clock::now() - start;
    require(elapsed < std::chrono::seconds(2), "Ordinary cache shutdown exceeded its bound");
    std::cout << "ordinary-thumbnail pending-source shutdown-ms="
              << std::chrono::duration<double, std::milli>(elapsed).count() << '\n';
}
}
int wmain(int argc, wchar_t **argv) {
    if (argc == 3 && std::wstring_view(argv[2]) == L"--shutdown") {
        try {
            shutdown_cases(std::filesystem::absolute(argv[1]));
            owner_shutdown();
            measure_thumbnail_shutdown();
            return 0;
        } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    }
    if (argc != 2)
        return 2;
    preview_worker::Handle token;
    DWORD contained{}, size{};
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) &&
        GetTokenInformation(token.value, TokenIsAppContainer, &contained, sizeof contained,
                            &size) &&
        contained) {
        std::cout << "Network access qualification requires a normal parent token\n";
        return 77;
    }
    try {
        const auto path = std::filesystem::absolute(argv[1]);
        auto sample = media_sample::dib(false);
        preview_worker::Request request;
        request.encoding = application::ScreenshotEncoding::dib;
        request.width = request.height = 128;
        request.length = static_cast<unsigned>(sample.bytes.size());
        auto deadline = [] { return std::chrono::steady_clock::now() + std::chrono::seconds(2); };
        for (auto serial : {201u, 202u, 203u, 205u, 206u, 207u, 208u}) {
            request.serial = serial;
            const auto start = std::chrono::steady_clock::now();
            rejects([&] { preview_worker::decode(request, sample.bytes, {}, deadline(), path); },
                    "Faulty worker result accepted");
            require(std::chrono::steady_clock::now() - start < std::chrono::seconds(2),
                    "Worker was not reclaimed within deadline");
        }
        request.serial = 201;
        std::stop_source stop;
        std::jthread cancel([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            stop.request_stop();
        });
        const auto start = std::chrono::steady_clock::now();
        rejects(
            [&] {
                preview_worker::decode(request, sample.bytes, stop.get_token(), deadline(), path);
            },
            "Running worker cancellation failed");
        require(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(700),
                "Running worker cancellation was slow");
        Directory directory;
        WSADATA network{};
        require(WSAStartup(MAKEWORD(2, 2), &network) == 0, "Network fixture unavailable");
        struct Socket {
            SOCKET value = INVALID_SOCKET;
            ~Socket() {
                if (value != INVALID_SOCKET)
                    closesocket(value);
            }
        };
        struct Network {
            ~Network() { WSACleanup(); }
        } network_cleanup;
        Socket listener{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        sockaddr_in endpoint{};
        endpoint.sin_family = AF_INET;
        endpoint.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        require(bind(listener.value, reinterpret_cast<sockaddr *>(&endpoint), sizeof endpoint) ==
                        0 &&
                    listen(listener.value, 1) == 0,
                "Local network listener unavailable");
        int endpoint_size = sizeof endpoint;
        require(getsockname(listener.value, reinterpret_cast<sockaddr *>(&endpoint),
                            &endpoint_size) == 0,
                "Listener port unavailable");
        {
            Socket control{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
            require(connect(control.value, reinterpret_cast<sockaddr *>(&endpoint),
                            sizeof endpoint) == 0,
                    "Parent could not reach test listener");
            Socket accepted{accept(listener.value, nullptr, nullptr)};
            require(accepted.value != INVALID_SOCKET, "Control connection missing");
        }
        u_long nonblocking = 1;
        ioctlsocket(listener.value, FIONBIO, &nonblocking);
        auto private_file = directory.path / "private-fixture.txt";
        std::ofstream(private_file) << "Synthetic private fixture";
        const auto name = private_file.wstring();
        for (auto c : name) {
            sample.bytes.push_back(static_cast<unsigned char>(c));
            sample.bytes.push_back(static_cast<unsigned char>(c >> 8));
        }
        sample.bytes.push_back(0);
        sample.bytes.push_back(0);
        const auto port = ntohs(endpoint.sin_port);
        sample.bytes.push_back(static_cast<unsigned char>(port));
        sample.bytes.push_back(static_cast<unsigned char>(port >> 8));
        request.length = static_cast<unsigned>(sample.bytes.size());
        request.serial = 204;
        const auto image = preview_worker::decode(request, sample.bytes, {}, deadline(), path);
        std::cout << "denied_access_mask=" << unsigned(image.pbgra[0]) << '\n';
        require(image.pbgra[0] == 7, "Sandbox file/network/child-process denial failed");
        Socket unexpected{accept(listener.value, nullptr, nullptr)};
        require(unexpected.value == INVALID_SOCKET && WSAGetLastError() == WSAEWOULDBLOCK,
                "Sandbox reached the controlled network endpoint");
        std::cout
            << "Preview timeout, crash, malformed IPC, cancellation and access boundaries passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
