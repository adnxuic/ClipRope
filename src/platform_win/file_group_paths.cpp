#include "platform_win/file_group_paths.hpp"
#include <shlobj.h>
#include <algorithm>
#include <cstring>

namespace cliprope {
namespace {
bool same_name(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
        b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

std::vector<unsigned char> copy_effect_bytes() {
    const DWORD effect = DROPEFFECT_COPY;
    std::vector<unsigned char> bytes(sizeof(effect));
    std::memcpy(bytes.data(), &effect, sizeof(effect));
    return bytes;
}

bool disk_path(std::wstring_view name) {
    if (name.empty() || name.size() > 32767) return false;
    const auto drive = [](std::wstring_view s) {
        return s.size() >= 3 && ((s[0] >= L'A' && s[0] <= L'Z') || (s[0] >= L'a' && s[0] <= L'z')) &&
               s[1] == L':' && (s[2] == L'\\' || s[2] == L'/');
    };
    if (drive(name)) return true;
    if (name.starts_with(L"\\\\.\\")) return false;
    if (name.starts_with(L"\\\\?\\")) {
        name.remove_prefix(4);
        if (drive(name)) return true;
        if (!same_name(name.substr(0, 4), L"UNC\\")) return false;
        name.remove_prefix(4);
    } else {
        if (!name.starts_with(L"\\\\")) return false;
        name.remove_prefix(2);
    }
    const auto server = name.find(L'\\');
    if (server == 0 || server == name.npos || server + 1 == name.size()) return false;
    name.remove_prefix(server + 1);
    const auto share = name.substr(0, name.find(L'\\'));
    return !share.empty() && !same_name(share, L"pipe") && !same_name(share, L"mailslot");
}

std::vector<unsigned char> encode_file_group(const std::vector<std::wstring>& paths) {
    DROPFILES header{};
    header.pFiles = sizeof header;
    header.fWide = TRUE;
    std::vector<unsigned char> bytes(sizeof header);
    std::memcpy(bytes.data(), &header, sizeof header);
    for (const auto& path : paths) {
        const auto* start = reinterpret_cast<const unsigned char*>(path.c_str());
        bytes.insert(bytes.end(), start, start + (path.size() + 1) * sizeof(wchar_t));
    }
    bytes.insert(bytes.end(), sizeof(wchar_t), 0);
    return bytes;
}
bool image_file_group(const std::vector<std::wstring>& paths) {
    return !paths.empty() && std::all_of(paths.begin(), paths.end(), [](const auto& path) {
        auto dot = path.find_last_of(L'.');
        if (dot == path.npos) return false;
        auto ext = std::wstring_view(path).substr(dot);
        return same_name(ext, L".png") || same_name(ext, L".jpg") || same_name(ext, L".jpeg") || same_name(ext, L".bmp");
    });
}

std::vector<std::wstring> file_group_paths(std::span<const unsigned char> bytes) {
    if (bytes.size() < sizeof(DROPFILES) || bytes.size() > max_group_bytes) return {};
    DROPFILES header{};
    std::memcpy(&header, bytes.data(), sizeof header);
    if (header.pFiles < sizeof header || header.pFiles >= bytes.size()) return {};
    const size_t unit = header.fWide ? 2 : 1;
    const auto input = bytes.subspan(header.pFiles);
    std::vector<std::wstring> paths;
    std::size_t start = 0;
    for (std::size_t end = 0; end + unit <= input.size(); end += unit) {
        if (input[end] || (unit == 2 && input[end + 1])) continue;
        if (end == start) return paths;
        if (paths.size() >= 1024 || (end - start) / unit > 32767) return {};
        std::wstring path;
        if (header.fWide) {
            path.resize((end - start) / 2);
            std::memcpy(path.data(), input.data() + start, end - start);
        } else {
            const auto* text = reinterpret_cast<const char*>(input.data() + start);
            const auto length = static_cast<int>(end - start);
            const int chars = MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text, length, nullptr, 0);
            if (!chars) return {};
            path.resize(chars);
            if (!MultiByteToWideChar(CP_ACP, MB_ERR_INVALID_CHARS, text, length, path.data(), chars)) return {};
        }
        if (!disk_path(path)) return {};
        paths.push_back(std::move(path));
        start = end + unit;
    }
    return {}; // Missing the final list terminator.
}

} // namespace cliprope
