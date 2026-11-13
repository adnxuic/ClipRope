#include "persistence/profile_location.hpp"
#include "preview_worker/protocol.hpp"
#include <windows.h>
#include <userenv.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string_view>

namespace fs = std::filesystem;
namespace {
bool ordinary(const fs::path &path) {
    const auto attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES)
        return !(attr & FILE_ATTRIBUTE_REPARSE_POINT);
    return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;
}
bool deletable(const fs::path &file) {
    const auto attr = GetFileAttributesW(file.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT) ||
        ((attr & FILE_ATTRIBUTE_READONLY) && !(attr & FILE_ATTRIBUTE_DIRECTORY)))
        return false;
    HANDLE h =
        CreateFileW(file.c_str(), DELETE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return false;
    CloseHandle(h);
    return true;
}
bool recognized_slots(const fs::path &root) {
    const std::wstring_view files[] = {L"ClipRope.exe",  L"ClipRopeWorker.exe", L"ClipRopePreviewWorker.exe", L"ClipRopeTargetWorker.exe", L"cliprope_headless.exe",
                                       L"Uninstall.exe", L"release.ini",        L"ThirdPartyNotices.txt"};
    for (const auto &slot : fs::directory_iterator(root)) {
        const auto name = slot.path().filename().wstring();
        if ((name != L"current" && name != L"previous" && name != L"incoming") || !ordinary(slot.path()) || !slot.is_directory() ||
            !deletable(slot.path()) || !fs::is_regular_file(slot.path() / L"release.ini"))
            return false;
        for (const auto &file : fs::directory_iterator(slot.path())) {
            auto filename = file.path().filename().wstring();
            if (!ordinary(file.path()) || !file.is_regular_file() ||
                std::find(std::begin(files), std::end(files), filename) == std::end(files) ||
                !deletable(file.path()))
                return false;
        }
    }
    return true;
}

bool same(const fs::path &a, const fs::path &b) {
    return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool contains_path(const fs::path &parent, fs::path child) {
    while (!same(child, parent)) {
        if (!child.has_relative_path()) return false;
        child = child.parent_path();
    }
    return true;
}

bool valid_component(const fs::path &part) {
    const auto &name = part.native();
    if (name.empty() || name.back() == L'.' || name.back() == L' ' ||
        name.find_first_of(L"<>:\"|?*") != name.npos ||
        std::any_of(name.begin(), name.end(), [](wchar_t ch) { return ch < 32; })) return false;
    auto stem = name.substr(0, name.find(L'.'));
    stem.erase(stem.find_last_not_of(L' ') + 1);
    for (const auto *device : {L"CON", L"PRN", L"AUX", L"NUL", L"CONIN$", L"CONOUT$"})
        if (same(stem, device)) return false;
    return !(stem.size() == 4 && std::wstring_view(L"0123456789\u00b9\u00b2\u00b3").find(stem.back()) != stem.npos &&
             (same(stem.substr(0, 3), L"COM") || same(stem.substr(0, 3), L"LPT")));
}

const char *check_destination(const fs::path &root, const fs::path &local, bool fresh) {
    if (!root.is_absolute() || root.root_name().native().size() != 2 || !root.has_relative_path())
        return "Choose a folder on a local drive, below the drive root.";
    const auto drive = GetDriveTypeW(root.root_path().c_str());
    if (drive != DRIVE_FIXED && drive != DRIVE_REMOVABLE && drive != DRIVE_RAMDISK)
        return "Choose a folder on a local drive.";
    if ((root / L"previous" / L"ClipRopePreviewWorker.exe").native().size() >= MAX_PATH)
        return "Choose a shorter installation folder path.";
    auto ancestor = root.root_path();
    for (const auto &part : root.relative_path()) {
        if (!valid_component(part)) return "Choose a valid folder name.";
        ancestor /= part;
        if (!ordinary(ancestor) || (fs::exists(ancestor) && !fs::is_directory(ancestor)))
            return "Choose an accessible folder without linked files or folders in its path.";
    }
    const auto target = fs::weakly_canonical(root);
    for (const auto &reserved : cliprope::persistence::ProfileLocation::reserved_directories(local)) {
        const auto path = fs::weakly_canonical(reserved);
        if (contains_path(path, target) || contains_path(target, path))
            return "Choose a folder separate from ClipRope history and settings.";
    }
    if (!fresh)
        return fs::is_directory(root) && !fs::is_empty(root) && recognized_slots(root) ? nullptr :
            "The registered installation contains missing or unrecognized package files.";
    if (fs::exists(root) && !fs::is_empty(root)) return "Choose an empty folder or enter a new folder name.";
    // Check the access token without creating a target or writing a probe on cancellation.
    while (!fs::exists(ancestor)) {
        if (!ancestor.has_relative_path()) return "The selected drive is unavailable.";
        ancestor = ancestor.parent_path();
    }
    HANDLE dir = CreateFileW(ancestor.c_str(), FILE_ADD_SUBDIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (dir == INVALID_HANDLE_VALUE) return "Choose a folder where your account can create folders.";
    CloseHandle(dir);
    return nullptr;
}

} // namespace
int wmain(int argc, wchar_t **argv) {
    if (argc == 4) {
        const std::wstring_view mode(argv[1]);
        if (mode != L"--check-new-install" && mode != L"--check-install-tree") return 2;
        try {
            // Setup appends a dot component so a trailing backslash never escapes the closing quote.
            const fs::path argument(argv[2]);
            const auto root = argument.filename() == L"." ? argument.parent_path() : argument;
            const auto *reason = check_destination(root, argv[3], mode == L"--check-new-install");
            if (!reason) return 0;
            std::puts(reason);
        } catch (const std::exception &) {
            std::puts("ClipRope could not verify the folder or read its history location. Check folder access and try again.");
        }
        return 1;
    }
    if (argc != 2)
        return 2;
    PWSTR path{};
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
        return 1;
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> release(path, CoTaskMemFree);
    const fs::path local(path);
    try {
        if (!ordinary(local))
            return 1;
        if (std::wstring_view(argv[1]) == L"--remove-preview-container") {
            const auto result = DeleteAppContainerProfile(cliprope::preview_worker::container_name);
            return SUCCEEDED(result) ? 0 : 1; // Windows also succeeds when the profile is absent.
        }
        if (std::wstring_view(argv[1]) == L"--confirm-delete-installed-history") {
            cliprope::persistence::ProfileLocation location(local);
            location.purge();
            return 0;
        }
    } catch (const std::exception &) {
        return 1;
    }
    return 2;
}
