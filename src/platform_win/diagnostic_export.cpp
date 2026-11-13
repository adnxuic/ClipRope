#include "platform_win/diagnostic_export.hpp"
#include <windows.h>
#include <stdexcept>

namespace cliprope::win {
void export_diagnostics(const std::filesystem::path &profile, std::string_view report) {
    const auto attrs = GetFileAttributesW(profile.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ||
        !(attrs & FILE_ATTRIBUTE_DIRECTORY) || report.size() > 4096)
        throw std::runtime_error("Invalid diagnostics destination");
    const auto temp = profile / (L"diagnostics-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
                                 std::to_wstring(GetTickCount64()) + L".tmp");
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot create diagnostics");
    DWORD written{};
    bool complete = WriteFile(h, report.data(), static_cast<DWORD>(report.size()), &written, nullptr) &&
                    written == report.size() && FlushFileBuffers(h);
    CloseHandle(h);
    if (!complete || !MoveFileExW(temp.c_str(), (profile / L"diagnostics.txt").c_str(),
                                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        throw std::runtime_error("Cannot finish diagnostics export");
    }
}
} // namespace cliprope::win
