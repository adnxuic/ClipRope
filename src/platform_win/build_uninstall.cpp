#include "platform_win/build_uninstall.hpp"
#include "platform_win/login_startup.hpp"
#include "persistence/profile_location.hpp"
#include <tlhelp32.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <vector>

namespace cliprope::win {
namespace fs = std::filesystem;
namespace {
constexpr auto marker = L"ClipRope.build.ini";
constexpr std::array payload{L"ClipRope.exe", L"ClipRopeWorker.exe", L"ClipRopePreviewWorker.exe", L"ClipRopeTargetWorker.exe",
                             L"cliprope_headless.exe", L"cliprope_profile_support.exe", L"Uninstall.exe"};
struct Close { void operator()(void* value) const { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
using Handle = std::unique_ptr<void, Close>;
struct Failure : std::runtime_error {
    UninstallIssue issue;
    std::wstring detail;
    DWORD code;
    Failure(UninstallIssue kind, std::wstring item = {}, DWORD error = 0)
        : std::runtime_error("Direct-build cleanup failed"), issue(kind), detail(std::move(item)), code(error) {}
};
bool same(const fs::path& a, const fs::path& b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool missing(DWORD error) { return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND; }

struct DirectoryGuard {
    std::vector<Handle> directories;
    explicit DirectoryGuard(const fs::path& root) {
        if (!root.is_absolute() || root == root.root_path() || root != root.lexically_normal())
            throw Failure(UninstallIssue::layout, root.native());
        auto current = root;
        for (;;) {
            Handle dir(CreateFileW(current.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (dir.get() == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(dir.get(), &info) ||
                !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
                throw Failure(UninstallIssue::layout, current.native(), GetLastError());
            directories.push_back(std::move(dir));
            if (current == current.root_path()) break;
            current = current.parent_path();
        }
    }
};
void layout(const fs::path& root) {
    DirectoryGuard guard(root);
    const auto file = root / marker;
    const auto attributes = GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)))
        throw Failure(UninstallIssue::layout, file.native());
    wchar_t kind[64]{};
    GetPrivateProfileStringW(L"ClipRope", L"Layout", L"", kind, DWORD(std::size(kind)), file.c_str());
    if (std::wstring_view(kind) != L"direct-build-v1") throw Failure(UninstallIssue::layout, file.native());
}
Handle removable(const fs::path& path) {
    Handle file(CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        if (missing(error)) return {};
        throw Failure(UninstallIssue::payload, path.native(), error);
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.get(), &info))
        throw Failure(UninstallIssue::payload, path.native(), GetLastError());
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_READONLY))
        throw Failure(UninstallIssue::payload, path.native(), ERROR_ACCESS_DENIED);
    return file;
}
bool installed_copy(const StartupRegistration& registration, const fs::path& executable) {
    return registration.installation && same(fs::path(*registration.installation) / L"current" / L"ClipRope.exe", executable);
}
} // namespace

ActiveBuildCopies active_build_copies(const fs::path& directory) {
    ActiveBuildCopies found;
    Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
    if (snapshot.get() == INVALID_HANDLE_VALUE) throw Failure(UninstallIssue::running, {}, GetLastError());
    PROCESSENTRY32W entry{sizeof(entry)};
    if (!Process32FirstW(snapshot.get(), &entry)) throw Failure(UninstallIssue::running, {}, GetLastError());
    do {
        if (entry.th32ProcessID == GetCurrentProcessId()) continue;
        if (CompareStringOrdinal(entry.szExeFile, -1, L"Uninstall.exe", -1, TRUE) == CSTR_EQUAL) continue;
        bool candidate{};
        for (auto name : payload)
            if (CompareStringOrdinal(entry.szExeFile, -1, name, -1, TRUE) == CSTR_EQUAL) candidate = true;
        if (!candidate) continue;
        Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID));
        if (!process) {
            // A process can disappear between snapshot and query.
            if (GetLastError() == ERROR_INVALID_PARAMETER) continue;
            throw Failure(UninstallIssue::running, entry.szExeFile, GetLastError());
        }
        wchar_t path[32768]{};
        DWORD length = DWORD(std::size(path));
        if (!QueryFullProcessImageNameW(process.get(), 0, path, &length))
            throw Failure(UninstallIssue::running, entry.szExeFile, GetLastError());
        if (same(fs::path(path).parent_path(), directory)) found.target = true;
        else if (CompareStringOrdinal(entry.szExeFile, -1, L"Uninstall.exe", -1, TRUE) != CSTR_EQUAL) found.other = true;
    } while (Process32NextW(snapshot.get(), &entry));
    return found;
}

BuildUninstall::BuildUninstall(BuildUninstallOptions options) : options_(std::move(options)) {}
BuildUninstallPreview BuildUninstall::preview() const {
    BuildUninstallPreview state{options_.directory};
    try {
        layout(options_.directory);
        LoginStartup startup(options_.directory / L"ClipRope.exe", options_.user);
        StartupRegistration saved;
        try { saved = startup.registration(); }
        catch (const std::exception&) { throw Failure(UninstallIssue::registry); }
        if (installed_copy(saved, options_.directory / L"ClipRope.exe"))
            throw Failure(UninstallIssue::layout, options_.directory.native());
        const auto active = options_.processes(options_.directory);
        state.foreign_registration = saved.conflict_for(options_.directory / L"ClipRope.exe").has_value();
        state.delete_history_available = !state.foreign_registration && !active.target && !active.other;
        if (active.target) state.issue = UninstallIssue::running;
    } catch (const Failure& error) {
        state.issue = error.issue; state.detail = error.detail; state.windows_error = error.code;
    } catch (const std::exception&) { state.issue = UninstallIssue::layout; }
    return state;
}
BuildUninstallResult BuildUninstall::remove(bool delete_history) const {
    BuildUninstallResult result;
    try {
        const auto state = preview();
        if (state.issue != UninstallIssue::none) throw Failure(state.issue, state.detail, state.windows_error);
        DirectoryGuard guard(options_.directory);
        if (options_.processes(options_.directory).target) throw Failure(UninstallIssue::running);
        if (delete_history && !state.delete_history_available) throw Failure(UninstallIssue::shared_history);
        // Acquire deletion rights for every known file before any registry/data mutation.
        std::vector<std::pair<fs::path, Handle>> files;
        for (auto name : payload) {
            const auto path = options_.directory / name;
            if (auto file = removable(path)) files.emplace_back(path, std::move(file));
        }
        auto marker_file = removable(options_.directory / marker);
        if (!marker_file) throw Failure(UninstallIssue::layout);
        files.emplace_back(options_.directory / marker, std::move(marker_file));
        LoginStartup startup(options_.directory / L"ClipRope.exe", options_.user);
        try { startup.check_removal_access(); }
        catch (const std::exception&) { throw Failure(UninstallIssue::registry); }
        StartupRegistration current;
        try { current = startup.registration(); }
        catch (const std::exception&) { throw Failure(UninstallIssue::registry); }
        if (delete_history && (current.conflict_for(options_.directory / L"ClipRope.exe") ||
                               options_.processes(options_.directory).other))
            throw Failure(UninstallIssue::shared_history);
        if (!current.conflict_for(options_.directory / L"ClipRope.exe") &&
            !options_.processes(options_.directory).other && options_.remove_preview) {
            try { options_.remove_preview(); }
            catch (const std::exception&) { throw Failure(UninstallIssue::preview); }
        }
        if (delete_history) {
            try {
                persistence::ProfileLocation location(options_.local_app_data);
                result.history_touched = true;
                location.purge();
            } catch (const std::exception&) { throw Failure(UninstallIssue::history); }
        }
        try { startup.remove_owned(); }
        catch (const std::exception&) { throw Failure(UninstallIssue::registry); }
        for (auto& [path, file] : files) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &disposition, sizeof(disposition)))
                throw Failure(UninstallIssue::payload, path.native(), GetLastError());
        }
        result.success = true;
    } catch (const Failure& error) {
        result.issue = error.issue; result.detail = error.detail; result.windows_error = error.code;
    } catch (const std::exception&) { result.issue = UninstallIssue::layout; }
    return result;
}
} // namespace cliprope::win
