#pragma once
#include <windows.h>
#include <filesystem>
#include <functional>
#include <string>

namespace cliprope::win {
enum class UninstallIssue { none, layout, running, shared_history, registry, payload, history, preview };
struct ActiveBuildCopies { bool target{}, other{}; };
ActiveBuildCopies active_build_copies(const std::filesystem::path& directory);
struct BuildUninstallOptions {
    std::filesystem::path directory, local_app_data;
    HKEY user = HKEY_CURRENT_USER;
    std::function<ActiveBuildCopies(const std::filesystem::path&)> processes = active_build_copies;
    std::function<void()> remove_preview;
};
struct BuildUninstallPreview {
    std::filesystem::path directory;
    bool delete_history_available{}, foreign_registration{};
    UninstallIssue issue = UninstallIssue::none;
    std::wstring detail;
    unsigned windows_error{};
};
struct BuildUninstallResult {
    bool success{}, history_touched{};
    UninstallIssue issue = UninstallIssue::none;
    std::wstring detail;
    unsigned windows_error{};
};
// Only explicit runtime filenames in a marked direct-build directory are removed.
// Registry and local-data roots are injectable so fixtures never touch a real profile.
class BuildUninstall final {
public:
    explicit BuildUninstall(BuildUninstallOptions options);
    BuildUninstallPreview preview() const;
    BuildUninstallResult remove(bool delete_history) const;
private:
    BuildUninstallOptions options_;
};
} // namespace cliprope::win
