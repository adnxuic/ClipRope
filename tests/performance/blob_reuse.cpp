#include "../integration/clipboard_process.hpp"
#include "../integration/history_fixture.hpp"
#include "persistence/history_store.hpp"
#include "rendering/image_preview.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <ole2.h>
#include <psapi.h>

namespace {
using namespace cliprope;
using history_fixture::require;
namespace fs = std::filesystem;

void seed(const fs::path& path) {
    BITMAPINFOHEADER h{sizeof h, 2048, 2048, 1, 24, BI_RGB, 2048 * 2048 * 3};
    std::vector<unsigned char> dib(sizeof h + h.biSizeImage);
    std::memcpy(dib.data(), &h, sizeof h);
    std::uint32_t state = 13579;
    for (auto& pixel : std::span(dib).subspan(sizeof h)) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        pixel = static_cast<unsigned char>(state);
    }
    domain::Format image;
    image.name = "PNG";
    image.bytes = rendering::encode_dib_png(CF_DIB, dib);
    require(image.bytes.size() >= 10u * 1024 * 1024, "Expected a large synthetic PNG");
    persistence::HistoryStore store(path);
    store.admit("main", domain::Snapshot{{std::move(image)}});
}

void measure(const fs::path& path, const fs::path& output) {
    persistence::HistoryStore store(path);
    const auto entry = store.hanging("main").at(0);
    const auto loaded = store.load(entry.id);
    const auto& png = loaded.snapshot.formats.at(0).bytes;
    const auto hash = persistence::sha256(png);
    std::ofstream csv(output);
    require(csv.good(), "Cannot write managed screenshot observations");
    csv << "series,trial,ms,png_bytes,private_bytes,process_peak_commit\n";
    fs::file_time_type published;
    fs::path original;
    for (int i = 0; i <= 20; ++i) {
        const auto begin = std::chrono::steady_clock::now();
        const auto file = store.materialize_screenshot(entry, png);
        const auto ms = std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now() - begin).count();
        PROCESS_MEMORY_COUNTERS_EX info{};
        info.cb = sizeof info;
        require(GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&info),
                                     sizeof info) != FALSE, "Cannot sample screenshot owner");
        csv << (i ? "managed_reuse" : "managed_first") << ',' << i << ',' << ms << ',' << png.size() << ','
            << info.PrivateUsage << ',' << info.PeakPagefileUsage << std::endl;
        if (!i) {
            original = file;
            published = fs::last_write_time(file) - std::chrono::hours(1);
            fs::last_write_time(file, published);
        }
        require(file == original && fs::last_write_time(file) == published, "Verified managed PNG was replaced");
    }
    require(persistence::file_sha256(original) == hash, "Managed screenshot bytes changed");
}
}

int wmain(int argc, wchar_t** argv) {
    if ((argc != 2 && argc != 4) || !probe::private_station() || FAILED(OleInitialize(nullptr))) return 2;
    int rc = 0;
    try {
        if (argc == 4) {
            require(std::wstring_view(argv[1]) == L"--measure", "Unrecognized measurement mode");
            measure(argv[2], argv[3]);
        } else {
            history_fixture::Directory temp;
            seed(temp.path);
            HANDLE child = probe::launch({fs::absolute(argv[0]).wstring(), L"--measure", temp.path.wstring(),
                                          fs::absolute(argv[1]).wstring()}, probe::desktop_path());
            require(child != nullptr, "Cannot launch fresh memory measurement");
            const auto wait = WaitForSingleObject(child, 60000);
            if (wait != WAIT_OBJECT_0) { TerminateProcess(child, 1); WaitForSingleObject(child, 3000); }
            DWORD code = 1;
            GetExitCodeProcess(child, &code);
            CloseHandle(child);
            require(wait == WAIT_OBJECT_0 && !code, "Managed screenshot measurement failed");
        }
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; rc = 1; }
    OleUninitialize();
    return rc;
}
