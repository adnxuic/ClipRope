#pragma once
#include "domain/image_preview.hpp"
#include "domain/history.hpp"
#include <windows.h>
#include <filesystem>
#include <string>

namespace cliprope::win {
// Clipboard paths are supplied by other processes. Scrolling past a card must
// not contact an SMB server or follow a junction to another volume.
inline bool needs_explicit_read(std::wstring_view name) {
    if (name.starts_with(L"\\\\?\\")) name.remove_prefix(4);
    if (name.size() < 3 || name[1] != L':') return true;
    const std::filesystem::path path(name);
    auto prefix = path.root_path();
    if (GetDriveTypeW(prefix.c_str()) == DRIVE_REMOTE) return true;
    for (const auto& part : path.relative_path()) {
        prefix /= part;
        const auto attrs = GetFileAttributesW(prefix.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) break;
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return true;
    }
    return false;
}
inline domain::PathKind local_path_kind(const std::wstring& path) {
    if (needs_explicit_read(path)) return domain::PathKind::unknown;
    const auto attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return domain::PathKind::unknown;
    return attrs & FILE_ATTRIBUTE_DIRECTORY ? domain::PathKind::directory : domain::PathKind::file;
}
// Probe only on the preview worker or an explicit file operation. Share access
// with editors and Explorer, and never hydrate an offline cloud placeholder.
inline domain::FileAvailability inspect_image_file(const std::wstring& path, WIN32_FILE_ATTRIBUTE_DATA& info) {
    using enum domain::FileAvailability;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        const auto error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            const auto root = std::filesystem::path(path).root_path();
            if (!root.empty() && GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES) return missing;
        }
        return inaccessible;
    }
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS |
                                 FILE_ATTRIBUTE_RECALL_ON_OPEN)) return inaccessible;
    if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_NO_RECALL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return inaccessible;
        CloseHandle(file);
    }
    return available;
}
} // namespace cliprope::win
