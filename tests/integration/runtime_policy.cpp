#include "host/runtime_policy.hpp"
#include <fstream>
#include <iostream>
#include <windows.h>

namespace fs = std::filesystem;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto root = fs::temp_directory_path() / (L"ClipRope-policy-" + std::to_wstring(GetCurrentProcessId()));
    bool owned{};
    try {
        check(!fs::exists(root), "Use an exclusive fixture directory");
        fs::create_directories(root / L"current");
        owned = true;
        const auto image = root / L"current" / L"ClipRope.exe";
        cliprope::win::StartupRegistration registration;
        check(!cliprope::host::installed_desktop(image, registration), "Loose copy was accepted");
        std::ofstream(root / L"current/release.ini") << "[ClipRope]\nVersion=test\n";
        check(!cliprope::host::installed_desktop(image, registration), "Marker alone was accepted");
        registration.installation = root.wstring();
        check(cliprope::host::installed_desktop(image, registration), "Registered package was rejected");
        check(!cliprope::host::installed_desktop(root / L"other/ClipRope.exe", registration), "Foreign image was accepted");
        std::ofstream(root / L"current/ClipRope.build.ini") << "[ClipRope]\nUsage=development\n";
        check(!cliprope::host::installed_desktop(image, registration), "Mixed development/package layout was accepted");
        // Launch the actual uninstalled desktop without a profile. It must exit before any UI or store opens.
        const std::wstring executable = argv[1];
        std::wstring command = L"\"" + executable + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process), "Launch failed");
        CloseHandle(process.hThread);
        const auto wait = WaitForSingleObject(process.hProcess, 10000);
        DWORD result{};
        GetExitCodeProcess(process.hProcess, &result);
        if (wait != WAIT_OBJECT_0) TerminateProcess(process.hProcess, 99);
        CloseHandle(process.hProcess);
        check(wait == WAIT_OBJECT_0 && result == 2, "Uninstalled startup did not reject missing profile");
        fs::remove_all(root);
        std::cout << "Installation identity and pre-storage development guard passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        if (owned) fs::remove_all(root);
        return 1;
    }
}
