#pragma once
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <windows.h>

namespace cliprope::win {
struct StartupRegistration {
    std::optional<std::wstring> owner, installation, command;
    std::optional<std::wstring> conflict_for(const std::filesystem::path& executable) const;
};
class StartupConflict final : public std::runtime_error {
public:
    explicit StartupConflict(std::wstring other)
        : std::runtime_error("Another copy of ClipRope owns the sign-in registration."), other_(std::move(other)) {}
    const std::wstring& other_executable() const { return other_; }
private:
    std::wstring other_;
};
// An explicit history profile has no LoginStartup instance.
class LoginStartup {
public:
    explicit LoginStartup(std::filesystem::path executable, HKEY user = HKEY_CURRENT_USER)
        : executable_(std::move(executable)), user_(user) {}
    // Adopt an older installation's current choice, or initialize a new direct run.
    void initialize() const;
    bool enabled() const;
    void set(bool enabled) const;
    StartupRegistration registration() const;
    // Preflight writes and remove only values that still identify this executable.
    void check_removal_access() const;
    void remove_owned() const;
private:
    std::filesystem::path executable_;
    HKEY user_;
};
}
