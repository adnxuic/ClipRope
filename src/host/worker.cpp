#include "capture_worker/protocol.hpp"
#include <limits>
#include <shellapi.h>

int WINAPI wWinMain(_In_ HINSTANCE, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int) {
    int argc{};
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const std::unique_ptr<void, decltype(&LocalFree)> arguments(argv, LocalFree);
    if (!argv) return 2;
    if (argc != 5) return 2;
    try {
        auto parse = [](std::wstring_view arg) {
            if (arg.empty() || arg.size() > 20 || arg.find_first_not_of(L"0123456789") != arg.npos)
                throw std::invalid_argument("Invalid worker argument");
            return std::stoull(std::wstring(arg));
        };
        const auto section = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(parse(argv[1])));
        const auto serial = parse(argv[2]), number = parse(argv[3]), deadline = parse(argv[4]);
        if (!section || !serial || !number || number > std::numeric_limits<DWORD>::max() ||
            !deadline || deadline > GetTickCount64() + 3000) return 2;
        const auto sequence = static_cast<DWORD>(number);
        if (FAILED(OleInitialize(nullptr))) return 1;
        struct Ole { ~Ole() { OleUninitialize(); } } ole;
        auto result = cliprope::capture_clipboard(sequence, cliprope::CaptureMode::local_history, deadline);
        cliprope::worker::write_response(section, serial, sequence, result);
        return 0;
    } catch (const std::exception&) { return 1; }
}
