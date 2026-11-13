#include "preview_worker/protocol.hpp"
#include "preview_worker/thumbnail.hpp"
#include "rendering/image_preview.hpp"
#include <shellapi.h>
#include <objbase.h>
#include <string_view>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using namespace cliprope::preview_worker;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::unique_ptr<void, decltype(&LocalFree)> args(argv, LocalFree);
    if (!argv || (argc != 4 && argc != 6))
        return 2;
    try {
        auto number = [](std::wstring_view s) {
            if (s.empty() || s.size() > 20 || s.find_first_not_of(L"0123456789") != s.npos)
                throw std::runtime_error("Invalid preview worker argument");
            return std::stoull(std::wstring(s));
        };
        Handle token;
        DWORD contained{}, size{};
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value) ||
            !GetTokenInformation(token.value, TokenIsAppContainer, &contained, sizeof contained, &size))
            return 3;
        if (argc == 6) {
            BOOL supervised{};
            if (contained || std::wstring_view(argv[1]) != L"--thumbnail" ||
                !IsProcessInJob(GetCurrentProcess(), nullptr, &supervised) || !supervised) return 3;
            Handle input(reinterpret_cast<HANDLE>(number(argv[2]))), output(reinterpret_cast<HANDLE>(number(argv[3]))),
                   metadata(reinterpret_cast<HANDLE>(number(argv[4])));
            if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return 5;
            const auto status = serve_thumbnail(input.value, output.value, metadata.value, number(argv[5]));
            CoUninitialize();
            return status;
        }
        if (!contained) return 3;
        Handle input(reinterpret_cast<HANDLE>(number(argv[1]))),
            output(reinterpret_cast<HANDLE>(number(argv[2])));
        const auto length = number(argv[3]);
        if (!length || length > input_limit)
            return 2;
        Mapping in(input.value, FILE_MAP_READ, sizeof(Request) + static_cast<size_t>(length));
        Mapping out(output.value, FILE_MAP_WRITE, output_size);
        if (!in.data || !out.data)
            return 4;
        Request request{};
        std::memcpy(&request, in.data, sizeof request);
        if (!valid(request) || request.length != length)
            return 2;
        if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
            return 5;
        struct Com {
            ~Com() { CoUninitialize(); }
        } com;
        try {
            auto image = cliprope::rendering::decode_screenshot_preview(
                format(request.encoding),
                {static_cast<const unsigned char *>(in.data) + sizeof(Request), request.length},
                request.width, request.height);
            Response response;
            response.serial = request.serial;
            response.status = 0;
            response.width = image.width;
            response.height = image.height;
            response.stride = image.width * 4;
            response.length = static_cast<unsigned>(image.pbgra.size());
            response.source_width = image.source.width;
            response.source_height = image.source.height;
            if (!valid(response, request))
                return 6;
            std::memcpy(static_cast<unsigned char *>(out.data) + sizeof(Response),
                        image.pbgra.data(), image.pbgra.size());
            std::memcpy(out.data, &response, sizeof response);
            return 0;
        } catch (...) {
            return 7;
        }
    } catch (...) {
        return 8;
    }
}
