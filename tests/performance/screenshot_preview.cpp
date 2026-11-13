#include "rendering/screenshot_preview_service.hpp"
#include "persistence/database.hpp"
#include <windows.h>
#include <psapi.h>
#include <fstream>
#include <iostream>
#include <thread>

using namespace cliprope;
int main(int argc, char **argv) {
    if (argc != 3)
        return 2;
    try {
        const auto seconds = std::stoul(argv[1]);
        if (seconds < 1 || seconds > 3600)
            return 2;
        std::ofstream out(argv[2]);
        if (!out)
            return 2;
        BITMAPINFOHEADER header{sizeof header, 4096, 2048, 1, 24, BI_RGB, 4096 * 2048 * 3};
        auto input = std::make_shared<std::vector<unsigned char>>(
            sizeof header + header.biSizeImage, static_cast<unsigned char>(120));
        std::memcpy(input->data(), &header, sizeof header);
        const auto hash = persistence::sha256(*input);
        rendering::ScreenshotPreviewService service([=](const auto &, auto, auto) {
            application::ScreenshotSource source;
            source.hash = hash;
            source.encoding = application::ScreenshotEncoding::dib;
            source.length = input->size();
            source.lease = input;
            source.read = [input](auto, auto) { return *input; };
            return source;
        });
        out << "elapsed_s,request_ms,cold,private_bytes,working_set,handles,gdi_objects,cache_"
               "bytes,decodes,hits\n";
        const auto start = std::chrono::steady_clock::now();
        std::uint64_t serial{};
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
            const bool cold = (serial % 10) == 0;
            service.cancel(cold);
            const auto begin = std::chrono::steady_clock::now();
            service.request({{"fixture", "snapshot", {}, {}, 1}, 1, ++serial, 2048, 1024});
            bool ready = false;
            while (std::chrono::steady_clock::now() - begin < std::chrono::seconds(3)) {
                if (auto result = service.take()) {
                    ready = bool(result->image);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            if (!ready)
                throw std::runtime_error("Soak preview failed");
            PROCESS_MEMORY_COUNTERS_EX memory{};
            memory.cb = sizeof memory;
            GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                                 sizeof memory);
            DWORD handles{};
            GetProcessHandleCount(GetCurrentProcess(), &handles);
            const auto count = service.counters();
            const auto now = std::chrono::steady_clock::now();
            out << std::chrono::duration<double>(now - start).count() << ','
                << std::chrono::duration<double, std::milli>(now - begin).count() << ',' << cold
                << ',' << memory.PrivateUsage << ',' << memory.WorkingSetSize << ',' << handles
                << ',' << GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS) << ',' << count.bytes
                << ',' << count.decodes << ',' << count.hits << '\n';
            out.flush();
            if (count.bytes > 16u * 1024 * 1024)
                throw std::runtime_error("Soak cache budget exceeded");
            if (serial % 60 == 0)
                std::cout << "soak_seconds=" << std::chrono::duration<double>(now - start).count()
                          << std::endl;
            std::this_thread::sleep_until(begin + std::chrono::seconds(1));
        }
        service.cancel();
        std::this_thread::sleep_for(std::chrono::seconds(32));
        if (service.counters().bytes)
            throw std::runtime_error("Idle cache was not reclaimed");
        std::cout << "Soak and idle reclamation passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
