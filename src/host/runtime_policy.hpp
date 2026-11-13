#pragma once
#include "platform_win/login_startup.hpp"
#include <filesystem>
#include <stdexcept>

namespace cliprope::host {
class DevelopmentProfileRequired final : public std::runtime_error {
public:
    DevelopmentProfileRequired() : std::runtime_error("Uninstalled ClipRope requires --profile. Use tools/build.ps1 -Action RunDev.") {}
};

// A package marker alone cannot grant access to the user's daily history.
inline bool installed_desktop(const std::filesystem::path& executable,
                              const win::StartupRegistration& registration) {
    if (!registration.installation) return false;
    const auto expected = (std::filesystem::path(*registration.installation) / L"current" / L"ClipRope.exe").lexically_normal();
    const auto actual = executable.lexically_normal();
    if (CompareStringOrdinal(expected.c_str(), -1, actual.c_str(), -1, TRUE) != CSTR_EQUAL) return false;
    const auto directory = actual.parent_path();
    return std::filesystem::is_regular_file(directory / L"release.ini") &&
           !std::filesystem::exists(directory / L"ClipRope.build.ini");
}
}
